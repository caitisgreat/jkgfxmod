#include "renderer.hpp"
#include "base/file_stream.hpp"
#include "base/log.hpp"
#include "base/memory_block.hpp"
#include "base/win32.hpp"
#include "d3d_impl.hpp"
#include "d3ddevice_impl.hpp"
#include "d3dexecutebuffer_impl.hpp"
#include "d3dviewport_impl.hpp"
#include "ddraw2_impl.hpp"
#include "ddraw_backbuffer_surface.hpp"
#include "ddraw_impl.hpp"
#include "ddraw_palette_impl.hpp"
#include "ddraw_phony_surface.hpp"
#include "ddraw_primary_surface.hpp"
#include "ddraw_sysmem_texture_surface.hpp"
#include "ddraw_vidmem_texture_surface.hpp"
#include "glad/glad.h"
#include "glutil/buffer.hpp"
#include "glutil/gl.hpp"
#include "glutil/program.hpp"
#include "glutil/shader.hpp"
#include "glutil/texture.hpp"
#include "glutil/vertex_array.hpp"
#include "math/colors.hpp"
#include <Windows.h>
#include <chrono>

namespace jkgm {
    LRESULT CALLBACK renderer_wndproc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }

    ATOM register_renderer_wndclass(HINSTANCE dll_instance)
    {
        WNDCLASSEX wc;
        ZeroMemory(&wc, sizeof(wc));

        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = renderer_wndproc;
        wc.hInstance = dll_instance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"wKernelAdvRenderer";

        return RegisterClassEx(&wc);
    }

    gl::shader compile_shader_from_file(fs::path const &filename, gl::shader_type type)
    {
        gl::shader rv(type);

        auto fs = make_file_input_stream(filename);
        memory_block mb;
        memory_output_block mob(&mb);
        fs->copy_to(&mob);

        gl::shader_source(rv, make_string_span(""), make_span(mb.str()));
        gl::compile_shader(rv);
        if(!gl::get_shader_compile_status(rv)) {
            LOG_ERROR(
                "Failed to compile ", filename.generic_string(), ": ", gl::get_shader_info_log(rv));
        }

        return rv;
    }

    void link_program_from_files(std::string const &name,
                                 gl::program *prog,
                                 fs::path const &vx,
                                 fs::path const &fg)
    {
        auto vx_shader = compile_shader_from_file(vx, gl::shader_type::vertex);
        auto fg_shader = compile_shader_from_file(fg, gl::shader_type::fragment);

        gl::attach_shader(*prog, vx_shader);
        gl::attach_shader(*prog, fg_shader);

        gl::link_program(*prog);

        if(!gl::get_program_link_status(*prog)) {
            LOG_ERROR("Failed to link program ", name, ": ", gl::get_program_info_log(*prog));
        }
    }

    struct opengl_state {
        gl::program menu_program;
        gl::program game_program;

        gl::vertex_array menu_vao;
        gl::buffer menu_vb;
        gl::buffer menu_ib;
        gl::texture menu_texture;

        std::vector<color_rgba8> menu_texture_data;

        opengl_state()
        {
            LOG_DEBUG("Loading OpenGL assets");

            link_program_from_files(
                "menu", &menu_program, "jkgm/shaders/menu.vert", "jkgm/shaders/menu.frag");
            link_program_from_files(
                "game", &game_program, "jkgm/shaders/game.vert", "jkgm/shaders/game.frag");

            gl::bind_vertex_array(menu_vao);

            std::array<point<2, float>, 4> menu_points{make_point(-1.0f, -1.0f),
                                                       make_point(1.0f, -1.0f),
                                                       make_point(-1.0f, 1.0f),
                                                       make_point(1.0f, 1.0f)};
            std::array<uint32_t, 6> menu_indices{0, 1, 2, 2, 1, 3};

            gl::enable_vertex_attrib_array(0U);
            gl::bind_buffer(gl::buffer_bind_target::array, menu_vb);
            gl::buffer_data(gl::buffer_bind_target::array,
                            make_span(menu_points).as_const_bytes(),
                            gl::buffer_usage::static_draw);
            gl::vertex_attrib_pointer(/*index*/ 0,
                                      /*elements*/ 2,
                                      gl::vertex_element_type::float32,
                                      /*normalized*/ false);

            gl::bind_buffer(gl::buffer_bind_target::element_array, menu_ib);
            gl::buffer_data(gl::buffer_bind_target::element_array,
                            make_span(menu_indices).as_const_bytes(),
                            gl::buffer_usage::static_draw);

            gl::bind_texture(gl::texture_bind_target::texture_2d, menu_texture);
            gl::tex_image_2d(gl::texture_bind_target::texture_2d,
                             0,
                             gl::texture_internal_format::rgba,
                             make_size(1024, 1024),
                             gl::texture_pixel_format::bgra,
                             gl::texture_pixel_type::uint8,
                             make_span<char const>(nullptr, 0U));
            gl::set_texture_max_level(gl::texture_bind_target::texture_2d, 0U);
            gl::set_texture_mag_filter(gl::texture_bind_target::texture_2d, gl::mag_filter::linear);

            menu_texture_data.resize(640 * 480, color_rgba8::zero());
        }
    };

    class renderer_impl : public renderer {
    private:
        DirectDraw_impl ddraw1;
        DirectDraw2_impl ddraw2;
        Direct3D_impl d3d1;
        Direct3DDevice_impl d3ddevice1;
        Direct3DViewport_impl d3dviewport1;

        DirectDraw_primary_surface_impl ddraw1_primary_surface;
        DirectDraw_backbuffer_surface_impl ddraw1_backbuffer_surface;

        std::vector<std::unique_ptr<DirectDrawPalette_impl>> ddraw1_palettes;
        std::vector<std::unique_ptr<DirectDraw_phony_surface_impl>> phony_surfaces;
        std::vector<std::unique_ptr<DirectDraw_sysmem_texture_surface_impl>>
            sysmem_texture_surfaces;
        std::vector<std::unique_ptr<DirectDraw_vidmem_texture_surface_impl>>
            vidmem_texture_surfaces;
        std::vector<std::unique_ptr<Direct3DExecuteBuffer_impl>> execute_buffers;

        HINSTANCE dll_instance;
        HWND hWnd;
        HDC hDC;
        HGLRC hGLRC;

        std::unique_ptr<opengl_state> ogs;

        char const *indexed_bitmap_source = nullptr;
        std::vector<color_rgba8> indexed_bitmap_colors;

    public:
        explicit renderer_impl(HINSTANCE dll_instance)
            : ddraw1(this)
            , ddraw2(this)
            , d3d1(this)
            , d3ddevice1(this)
            , d3dviewport1(this)
            , ddraw1_primary_surface(this)
            , ddraw1_backbuffer_surface(this)
            , dll_instance(dll_instance)
        {
            indexed_bitmap_colors.resize(256, color_rgba8::zero());
            register_renderer_wndclass(dll_instance);
        }

        void initialize() override
        {
            hWnd = CreateWindow(L"wKernelAdvRenderer",
                                L"JkGfxMod",
                                WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                700,
                                300,
                                1920,
                                1440,
                                /*parent*/ NULL,
                                /*menu*/ NULL,
                                dll_instance,
                                /*param*/ NULL);

            hDC = GetDC(hWnd);

            PIXELFORMATDESCRIPTOR pfd;
            ZeroMemory(&pfd, sizeof(pfd));

            pfd.nSize = sizeof(pfd);
            pfd.nVersion = 1;
            pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            pfd.iPixelType = PFD_TYPE_RGBA;
            pfd.cColorBits = 32;
            pfd.cAlphaBits = 8;
            pfd.cDepthBits = 24;

            int pfdid = ChoosePixelFormat(hDC, &pfd);
            if(pfdid == 0) {
                LOG_ERROR("Renderer ChoosePixelFormat failed: ",
                          win32::win32_category().message(GetLastError()));
            }

            SetPixelFormat(hDC, pfdid, &pfd);

            hGLRC = wglCreateContext(hDC);
            wglMakeCurrent(hDC, hGLRC);

            if(!gladLoadGL()) {
                LOG_ERROR("Failed to load GLAD");
            }

            ShowWindow(hWnd, SW_SHOW);

            gl::set_clear_color(solid(colors::cornflower_blue));
            gl::clear({gl::clear_flag::color, gl::clear_flag::depth});

            SwapBuffers(hDC);

            ogs = std::make_unique<opengl_state>();
        }

        HRESULT enumerate_devices(LPDDENUMCALLBACKA cb, LPVOID lpContext) override
        {
            // Emit only a single device, the default system device
            std::string fullname = "JkGfxMod OpenGL Device";
            std::string shortname = "device";
            cb(NULL, &fullname[0], &shortname[0], lpContext);

            return DD_OK;
        }

        void set_menu_source(char const *indexed_bitmap) override
        {
            indexed_bitmap_source = indexed_bitmap;
        }

        void set_menu_palette(UINT start, span<RGBQUAD const> entries) override
        {
            UINT curr = start;
            for(auto const &em : entries) {
                if(curr > 255U) {
                    break;
                }

                indexed_bitmap_colors[curr++] =
                    color_rgba8(em.rgbRed, em.rgbGreen, em.rgbBlue, uint8_t(255U));
            }
        }

        void present_menu() override
        {
            gl::clear({gl::clear_flag::color, gl::clear_flag::depth});

            // Copy new data from menu source
            if(indexed_bitmap_source) {
                for(size_t idx = 0U; idx < ogs->menu_texture_data.size(); ++idx) {
                    uint8_t index = indexed_bitmap_source[idx];
                    ogs->menu_texture_data[idx] = indexed_bitmap_colors[index];
                }
            }

            // Blit texture data into texture
            gl::set_active_texture_unit(0);
            gl::bind_texture(gl::texture_bind_target::texture_2d, ogs->menu_texture);
            gl::tex_sub_image_2d(gl::texture_bind_target::texture_2d,
                                 0,
                                 make_box(make_point(0, 0), make_point(640, 480)),
                                 gl::texture_pixel_format::rgba,
                                 gl::texture_pixel_type::uint8,
                                 make_span(ogs->menu_texture_data).as_const_bytes());

            // Render
            gl::use_program(ogs->menu_program);
            gl::set_uniform_integer(gl::uniform_location_id(0), 0);

            gl::bind_vertex_array(ogs->menu_vao);
            gl::draw_elements(gl::element_type::triangles, 6U, gl::index_type::uint32);

            SwapBuffers(hDC);
        }

        void execute_game(IDirect3DExecuteBuffer *cmdbuf, IDirect3DViewport *vp) override
        {
            gl::enable(gl::capability::depth_test);
            gl::enable(gl::capability::blend);
            gl::set_blend_function(gl::blend_function::one,
                                   gl::blend_function::one_minus_source_alpha);
            gl::set_depth_function(gl::comparison_function::less);
            gl::use_program(ogs->game_program);
            gl::set_uniform_integer(gl::uniform_location_id(0), 0);

            D3DEXECUTEDATA ed;
            cmdbuf->GetExecuteData(&ed);

            D3DEXECUTEBUFFERDESC ebd;
            cmdbuf->Lock(&ebd);

            D3DVIEWPORT vpd;
            vp->GetViewport(&vpd);

            auto vertex_span =
                make_span((D3DTLVERTEX const *)((char const *)ebd.lpData + ed.dwVertexOffset),
                          ed.dwVertexCount);

            auto cmd_span = make_span((char const *)ebd.lpData + ed.dwInstructionOffset,
                                      ed.dwInstructionLength);

            while(!cmd_span.empty()) {
                D3DINSTRUCTION inst = *(D3DINSTRUCTION const *)cmd_span.data();
                cmd_span = cmd_span.subspan(sizeof(D3DINSTRUCTION), span_to_end);

                for(size_t i = 0; i < inst.wCount; ++i) {
                    switch(inst.bOpcode) {
                    case D3DOP_EXIT:
                        break;

                    case D3DOP_PROCESSVERTICES: {
                        auto const *payload = (D3DPROCESSVERTICES const *)cmd_span.data();
                        if(payload->dwFlags != D3DPROCESSVERTICES_COPY || payload->wStart != 0 ||
                           payload->wDest != 0) {
                            LOG_ERROR("Unimplemented process vertices opcode ignored: ",
                                      payload->dwFlags,
                                      " ",
                                      payload->dwCount,
                                      " ",
                                      payload->wStart,
                                      " ",
                                      payload->wDest);
                            abort();
                        }
                    } break;

                    case D3DOP_STATERENDER: {
                        auto const *payload = (D3DSTATE const *)cmd_span.data();
                        switch(payload->drstRenderStateType) {
                        case D3DRENDERSTATE_TEXTUREHANDLE:
                            gl::bind_texture(
                                gl::texture_bind_target::texture_2d,
                                vidmem_texture_surfaces.at(payload->dwArg[0])->ogl_texture);
                            break;

                        // Silently ignore some useless commands
                        case D3DRENDERSTATE_ANTIALIAS:
                        case D3DRENDERSTATE_TEXTUREPERSPECTIVE:
                        case D3DRENDERSTATE_FILLMODE:
                        case D3DRENDERSTATE_SHADEMODE:
                        case D3DRENDERSTATE_MONOENABLE:
                        case D3DRENDERSTATE_TEXTUREMAPBLEND:
                        case D3DRENDERSTATE_TEXTUREMAG:
                        case D3DRENDERSTATE_TEXTUREMIN:
                        case D3DRENDERSTATE_SRCBLEND:
                        case D3DRENDERSTATE_DESTBLEND:
                        case D3DRENDERSTATE_CULLMODE:
                        case D3DRENDERSTATE_ZFUNC:
                        case D3DRENDERSTATE_ALPHAFUNC:
                        case D3DRENDERSTATE_DITHERENABLE:
                        case D3DRENDERSTATE_ALPHABLENDENABLE:
                        case D3DRENDERSTATE_FOGENABLE:
                        case D3DRENDERSTATE_SPECULARENABLE:
                        case D3DRENDERSTATE_SUBPIXEL:
                        case D3DRENDERSTATE_SUBPIXELX:
                        case D3DRENDERSTATE_STIPPLEDALPHA:
                            break;

                        default:
                            LOG_WARNING("Ignored unknown state render opcode: ",
                                        (int)payload->dtstTransformStateType);
                            break;
                        }
                    } break;

                    case D3DOP_TRIANGLE: {
                        auto const *payload = (D3DTRIANGLE const *)cmd_span.data();

                        auto const &v1 = vertex_span.data()[payload->v1];
                        auto const &v2 = vertex_span.data()[payload->v2];
                        auto const &v3 = vertex_span.data()[payload->v3];

                        // HACK:
                        ::glBegin(GL_TRIANGLES);

                        auto r1 = float(RGBA_GETRED(v1.color)) / 255.0f;
                        auto g1 = float(RGBA_GETGREEN(v1.color)) / 255.0f;
                        auto b1 = float(RGBA_GETBLUE(v1.color)) / 255.0f;
                        auto a1 = float(RGBA_GETALPHA(v1.color)) / 255.0f;

                        ::glColor4f(r1, g1, b1, a1);
                        ::glTexCoord2f(v1.tu, v1.tv);
                        ::glVertex4f(v1.sx, v1.sy, v1.sz, v1.rhw);

                        auto r2 = float(RGBA_GETRED(v2.color)) / 255.0f;
                        auto g2 = float(RGBA_GETGREEN(v2.color)) / 255.0f;
                        auto b2 = float(RGBA_GETBLUE(v2.color)) / 255.0f;
                        auto a2 = float(RGBA_GETALPHA(v2.color)) / 255.0f;

                        ::glColor4f(r2, g2, b2, a2);
                        ::glTexCoord2f(v2.tu, v2.tv);
                        ::glVertex4f(v2.sx, v2.sy, v2.sz, v2.rhw);

                        auto r3 = float(RGBA_GETRED(v3.color)) / 255.0f;
                        auto g3 = float(RGBA_GETGREEN(v3.color)) / 255.0f;
                        auto b3 = float(RGBA_GETBLUE(v3.color)) / 255.0f;
                        auto a3 = float(RGBA_GETALPHA(v3.color)) / 255.0f;

                        ::glColor4f(r3, g3, b3, a3);
                        ::glTexCoord2f(v3.tu, v3.tv);
                        ::glVertex4f(v3.sx, v3.sy, v3.sz, v3.rhw);
                        ::glEnd();
                    } break;

                    default:
                        LOG_WARNING(
                            "Unimplemented execute buffer opcode ", inst.bOpcode, " was ignored");
                    }

                    cmd_span = cmd_span.subspan(inst.bSize, span_to_end);
                }
            }

            cmdbuf->Unlock();
        }

        void present_game() override
        {
            SwapBuffers(hDC);
            gl::clear({gl::clear_flag::color, gl::clear_flag::depth});
        }

        IDirectDraw *get_directdraw() override
        {
            return &ddraw1;
        }

        IDirectDraw2 *get_directdraw2() override
        {
            return &ddraw2;
        }

        IDirect3D *get_direct3d() override
        {
            return &d3d1;
        }

        IDirect3DDevice *get_direct3ddevice() override
        {
            return &d3ddevice1;
        }

        IDirect3DViewport *get_direct3dviewport() override
        {
            return &d3dviewport1;
        }

        IDirectDrawSurface *get_directdraw_primary_surface() override
        {
            return &ddraw1_primary_surface;
        }

        IDirectDrawSurface *get_directdraw_backbuffer_surface() override
        {
            return &ddraw1_backbuffer_surface;
        }

        IDirectDrawSurface *get_directdraw_phony_surface(DDSURFACEDESC desc,
                                                         std::string name) override
        {
            phony_surfaces.push_back(
                std::make_unique<DirectDraw_phony_surface_impl>(this, desc, std::move(name)));
            return phony_surfaces.back().get();
        }

        IDirectDrawSurface *get_directdraw_sysmem_texture_surface(DDSURFACEDESC desc) override
        {
            sysmem_texture_surfaces.push_back(
                std::make_unique<DirectDraw_sysmem_texture_surface_impl>(this, desc));
            return sysmem_texture_surfaces.back().get();
        }

        IDirectDrawSurface *get_directdraw_vidmem_texture_surface(DDSURFACEDESC desc) override
        {
            vidmem_texture_surfaces.push_back(
                std::make_unique<DirectDraw_vidmem_texture_surface_impl>(
                    this, desc, vidmem_texture_surfaces.size()));
            return vidmem_texture_surfaces.back().get();
        }

        IDirectDrawPalette *get_directdraw_palette(span<PALETTEENTRY const> entries) override
        {
            ddraw1_palettes.push_back(std::make_unique<DirectDrawPalette_impl>(this));

            auto *rv = ddraw1_palettes.back().get();
            std::copy(entries.begin(), entries.end(), rv->entries.begin());

            return rv;
        }

        IDirect3DExecuteBuffer *get_direct3dexecutebuffer(size_t bufsz) override
        {
            execute_buffers.push_back(std::make_unique<Direct3DExecuteBuffer_impl>(this, bufsz));
            return execute_buffers.back().get();
        }
    };
}

std::unique_ptr<jkgm::renderer> jkgm::create_renderer(HINSTANCE dll_instance)
{
    return std::make_unique<jkgm::renderer_impl>(dll_instance);
}