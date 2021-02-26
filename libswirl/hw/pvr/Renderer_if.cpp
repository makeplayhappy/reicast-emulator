/*
	This file is part of libswirl
*/
#include "license/bsd"


#include <imgui/imgui.h>
#include "types.h"
#include "gui/gui_partials.h"

#include "Renderer_if.h"
#include "ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/TexCache.h"
#include "gui/gui.h"
#include "gui/gui_renderer.h"

#include "deps/crypto/md5.h"

#include "scripting/lua_bindings.h"
#include "reios/reios.h"

#include <memory>
#include <atomic>
#include <iterator>

#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>

#if FEAT_HAS_NIXPROF
#include "profiler/profiler.h"
#endif

#define FRAME_MD5 0x1
FILE* fLogFrames;
FILE* fCheckFrames;

/*

	rendv3 ideas
	- multiple backends
	  - ESish
	    - OpenGL ES2.0
	    - OpenGL ES3.0
	    - OpenGL 3.1
	  - OpenGL 4.x
	  - Direct3D 10+ ?
	- correct memory ordering model
	- resource pools
	- threaded ta
	- threaded rendering
	- rtts
	- framebuffers
	- overlays


	PHASES
	- TA submition (memops, dma)

	- TA parsing (defered, rend thread)

	- CORE render (in-order, defered, rend thread)


	submition is done in-order
	- Partial handling of TA values
	- Gotchas with TA contexts

	parsing is done on demand and out-of-order, and might be skipped
	- output is only consumed by renderer

	render is queued on RENDER_START, and won't stall the emulation or might be skipped
	- VRAM integrity is an issue with out-of-order or delayed rendering.
	- selective vram snapshots require ta parsing to complete in order with REND_START / REND_END


	Complications
	- For some apis (gles2, maybe gl31) texture allocation needs to happen on the gpu thread
	- multiple versions of different time snapshots of the same texture are required
	- ta parsing vs frameskip logic


	Texture versioning and staging
	 A memory copy of the texture can be used to temporary store the texture before upload to vram
	 This can be moved to another thread
	 If the api supports async resource creation, we don't need the extra copy
	 Texcache lookups need to be versioned


	rendv2x hacks
	- Only a single pending render. Any renders while still pending are dropped (before parsing)
	- wait and block for parse/texcache. Render is async
*/

u32 VertexCount=0;
u32 FrameCount=1;

static unique_ptr<Renderer> renderer;
static unique_ptr<Renderer> fallback_renderer;
//bool renderer_enabled = true;	// Signals the renderer thread to exit
bool renderer_changed = false;	// Signals the renderer thread to switch renderer

static atomic<bool> pend_rend(false);
static cResetEvent rs, re;

int max_idx,max_mvo,max_op,max_pt,max_tr,max_vtx,max_modt, ovrn;

static bool render_called = false;

TA_context* _pvrrc;
void SetREP(TA_context* cntx);
void killtex();
bool render_output_framebuffer();

bool dump_frame_switch = false;
bool dump_verts_switch = false;
int render_counter = 1;
// auto or slug
//vulkan
//gl41
//gles2
//soft
//softref
//none
static std::map<const string, rendererbackend_t>* p_backends;
#define backends (*p_backends)


static void rend_create_renderer(u8* vram)
{
    if (backends.count(settings.pvr.backend))
    {
        printf("RendIF: renderer: %s\n", settings.pvr.backend.c_str());
        renderer.reset(backends[settings.pvr.backend].create(vram));
        renderer->backendInfo = backends[settings.pvr.backend];
    }
    else
    {
        vector<rendererbackend_t> vec = rend_get_backends();

        auto main = (*vec.begin());

        renderer.reset(main.create(vram));
        renderer->backendInfo = main;

        if ((++vec.begin()) != vec.end())
        {
            auto fallback = (*(++vec.begin()));
            fallback_renderer.reset(fallback.create(vram));
            fallback_renderer->backendInfo = fallback;
        }

        printf("RendIF: renderer (auto): ");
        printf("main: %s", (vec.begin())->slug.c_str());
        if (fallback_renderer)
            printf(" fallback: %s", (++vec.begin())->slug.c_str());
        printf("\n");
    }
}

void rend_init_renderer(u8* vram)
{
    if (renderer == NULL)
        rend_create_renderer(vram);

    if (!renderer->Init())
    {
        printf("RendIF: Renderer %s did not initialize. Falling back to %s.\n",
            renderer->backendInfo.slug.c_str(),
            fallback_renderer->backendInfo.slug.c_str()
        );

        renderer = std::move(fallback_renderer);

        if (renderer == NULL || !renderer->Init())
        {
			renderer.reset();
            die("RendIF: Renderer initialization failed\n");
        }
    }

	printf("RendIF: Using renderer: %s\n", renderer->backendInfo.slug.c_str());
}

void rend_term_renderer()
{
    killtex();

	renderer.reset();
	fallback_renderer.reset();
}

static bool rend_frame(u8* vram, TA_context* ctx) {
//////#if FIXME
    if (dump_frame_switch){
		// || dump_verts_switch) {
		bool dodump = true;
		//build path to dump
		std::string base_dump_dir = get_writable_data_path(DATA_PATH "frame_dump/");

		if (!file_exists(base_dump_dir))
			make_directory(base_dump_dir);

		std::string game_id = reios_product_number;
		const size_t str_end = game_id.find_last_not_of(" ");
		if (str_end == std::string::npos)
			dodump = false;

		game_id = game_id.substr(0, str_end + 1);
		std::replace(game_id.begin(), game_id.end(), ' ', '_');

		//std::string game_id = GetGameId();
		if (game_id.length() == 0)
		dodump = false;

		base_dump_dir += game_id + "/";
		if (!file_exists(base_dump_dir))
			make_directory(base_dump_dir);

		std::stringstream path;

		time_t rawtime;
		struct tm * timeinfo;
		char tbuffer [22];

		time (&rawtime);
		timeinfo = localtime (&rawtime);
		
		//Issue - seem to get the null character added in to the tbuffer even though sizes / maxsize should scrub it
		//by adding in a "--" at the end and setting max size to remove it seems to work 
		if( dump_frame_switch ){
			strftime (tbuffer,22,"dc-%j-%H-%M-%S-frame--",timeinfo); //%j%H%M%S 3+2+2+2 = 9
		//rc = strcat(txt,timestamp);
			path << base_dump_dir << std::string(tbuffer) << std::hex << FrameCount << ".vram";
		}

		//if( dump_verts_switch ){
		//	path << base_dump_dir;// << std::string(tbuffer) << std::hex << FrameCount << ".vram";
		//}
		basic_string<char> path_str = path.str();

		if(dodump ){
			if(dump_frame_switch){
        		tactx_write_frame(path_str, ctx, &vram[0]);
			}
			
		}
        dump_frame_switch = false;
		
    }
	// endif dump_frame_switch


////#endif

    if (renderer_changed)
    {
        renderer_changed = false;
        rend_term_renderer();
    }

    if (renderer == nullptr) {
        rend_init_renderer(vram);
    }

    bool proc = true;

	if (ctx) {
		proc = renderer->Process(ctx);
		if (!proc || !ctx->rend.isRTT) {
			// If rendering to texture, continue locking until the frame is rendered
			pend_rend = false;
			re.Set();
		}
	}

    bool do_swp = proc && renderer->RenderPVR();

    return do_swp;
}

namespace {


    
#if 0
static bool rend_single_frame()
{
	if (renderer_changed)
	{
		renderer_changed = false;
		rend_term_renderer();
	}

    if (renderer == null) {
        rend_create_renderer();
        rend_init_renderer();
    }

    if (g_GUI->IsOpen() || g_GUI->IsVJoyEdit())
    {
        os_DoEvents();

        g_GUI->RenderUI();

        if (g_GUI->IsVJoyEdit() && renderer != NULL)
            renderer->DrawOSD(true);

        FinishRender(NULL);
        // Use the rendering start event to wait between two frames but save its value
        if (rs.Wait(17))
            rs.Set();
        return true;
    }

    //wait render start only if no frame pending
	do
	{
		// FIXME not here
		os_DoEvents();

		luabindings_onframe();

		{
			if (renderer != NULL)
				renderer->RenderLastFrame();

			if (!rs.Wait(1))
				return false;
		}

        if (!renderer_enabled)
			return false;

		_pvrrc = DequeueRender();
	}
	while (!_pvrrc);
	bool do_swp = rend_frame(_pvrrc, true);

	if (_pvrrc->rend.isRTT)
		re.Set();

	//clear up & free data ..
	FinishRender(_pvrrc);
	_pvrrc=0;

	return do_swp;
}
#endif

#if 0
static void* rend_thread(void* p)
{
	rend_init_renderer();

	//we don't know if this is true, so let's not speculate here
	//renderer->Resize(640, 480);

	while (renderer_enabled)
	{
		if (rend_single_frame())
			renderer->Present();
	}

	rend_term_renderer();

	return NULL;
}


static void rend_stop_renderer()
{
    renderer_enabled = false;
    tactx_Term();
}


static void rend_cancel_emu_wait()
{
    FinishRender(NULL);

    re.Set();
}
#endif

}


//called always on vlbank, even before rend_init_renderer
void rend_resize(int width, int height) {
    screen_width = width;
    screen_height = height;
	if (renderer) renderer->Resize(width, height);
}


void rend_start_render(u8* vram)
{
	render_called = true;

	#if FEAT_TA == TA_HLE
		pend_rend = false;
	#else
		// make sure no fb write is pending
		if (pend_rend) {
			re.Wait();
			pend_rend = false;
		}
	#endif
	
	TA_context* ctx = tactx_Pop(CORE_CURRENT_CTX);

	if (ctx)
	{
        SetREP(ctx);
		bool is_rtt=(FB_W_SOF1& 0x1000000)!=0;
		
		if (fLogFrames || fCheckFrames) {
			MD5Context md5;
			u8 digest[16];

			MD5Init(&md5);
			MD5Update(&md5, ctx->tad.thd_root, (unsigned)(ctx->tad.End() - ctx->tad.thd_root));
			MD5Final(digest, &md5);

			if (fLogFrames) {
				fputc(FRAME_MD5, fLogFrames);
				fwrite(digest, 1, 16, fLogFrames);
				fflush(fLogFrames);
			}

			if (fCheckFrames) {
				u8 digest2[16];
				int ch = fgetc(fCheckFrames);

				if (ch == EOF) {
					printf("Testing: TA Hash log matches, exiting\n");
					exit(1);
				}
				
				verify(ch == FRAME_MD5);

				fread(digest2, 1, 16, fCheckFrames);

				verify(memcmp(digest, digest2, 16) == 0);

				
			}

			/*
			u8* dig = digest;
			printf("FRAME: %02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X\n",
				digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
				digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]
				);
			*/
		}

		if (!ctx->rend.Overrun)
		{
			//tactx_Recycle(ctx); ctx = read_frame("frames/dcframe-SoA-intro-tr-autosort");
			//printf("REP: %.2f ms\n",render_end_pending_cycles/200000.0);
			
			FillBGP(vram, ctx);
			
			ctx->rend.isRTT=is_rtt;

			ctx->rend.fb_X_CLIP=FB_X_CLIP;
			ctx->rend.fb_Y_CLIP=FB_Y_CLIP;
			
			ctx->rend.fog_clamp_min = FOG_CLAMP_MIN;
			ctx->rend.fog_clamp_max = FOG_CLAMP_MAX;
			
			max_idx=max(max_idx,ctx->rend.idx.used());
			max_vtx=max(max_vtx,ctx->rend.verts.used());
			max_op=max(max_op,ctx->rend.global_param_op.used());
			max_pt=max(max_pt,ctx->rend.global_param_pt.used());
			max_tr=max(max_tr,ctx->rend.global_param_tr.used());
			
			max_mvo=max(max_mvo,ctx->rend.global_param_mvo.used());
			max_modt=max(max_modt,ctx->rend.modtrig.used());

#if HOST_OS==OS_WINDOWS && 0
			printf("max: idx: %d, vtx: %d, op: %d, pt: %d, tr: %d, mvo: %d, modt: %d, ov: %d\n", max_idx, max_vtx, max_op, max_pt, max_tr, max_mvo, max_modt, ovrn);
#endif
			if (QueueRender(ctx))
			{
				palette_update();

                g_GUIRenderer->QueueEmulatorFrame([=](){

                    _pvrrc = DequeueRender();
                    
                    verify(_pvrrc == ctx);
                    
                    bool do_swp = rend_frame(vram, _pvrrc);

					if (_pvrrc->rend.isRTT) {
						pend_rend = false;
                        re.Set();
					}

                    //clear up & free data ..
                    FinishRender(_pvrrc);
                    _pvrrc = 0;

                    return do_swp;
                });

				pend_rend = true;
				rs.Set();

			}
		}
		else
		{
			ovrn++;
			printf("WARNING: Rendering context is overrun (%d), aborting frame\n",ovrn);
			tactx_Recycle(ctx);
		}
	}
	else
	{
		SetREP(nullptr);
		palette_update();
		g_GUIRenderer->QueueEmulatorFrame([=](){
			bool do_swp = rend_frame(vram, nullptr);

			//pend_rend = false;
			re.Set();

			//clear up & free data ..
			FinishRender(nullptr);

			return do_swp;
		});

		pend_rend = true;
		rs.Set();
	}
}

void rend_end_render()
{
#if 1 //also disabled the printf, it takes quite some time ...
	#if HOST_OS!=OS_WINDOWS && !(defined(_ANDROID) || defined(TARGET_PANDORA))
		//too much console spam.
		//TODO: how about a counter?
		//if (!re.state) printf("Render > Extended time slice ...\n");
	#endif
#endif

	if (pend_rend) {
		re.Wait();
		#if FEAT_TA == TA_LLE
			pend_rend = false;
		#endif
	}
}

void rend_vblank()
{
	#if FEAT_TA == TA_HLE
		if (!render_called && fb_dirty && FB_R_CTRL.fb_enable)
	#else
		fb_dirty = true;
		if (fb_dirty && FB_R_CTRL.fb_enable)
	#endif
	{
        fb_dirty = false;
		#if FEAT_TA == TA_LLE
		if (pend_rend) {
			re.Wait();
			pend_rend = false;
		}
		pend_rend = true;
		#endif

        g_GUIRenderer->QueueEmulatorFrame([] () {
			#if FEAT_TA == TA_LLE
				re.Set();
			#endif
			// TODO: FIXME Actually check and re init this. Better yet, refactor
            if (renderer)
			{
                return renderer->RenderFramebuffer();
            }
            return true;
        });
	}
	render_called = false;
    pvr_update_framebuffer_watches();
	reset_dump_switches();

}


void reset_dump_switches(){
///std::to_string(render_counter)
	
	
	if( dump_verts_switch ){
		printf(" ::resetDumpSwitches:: %d", render_counter);
		dump_verts_switch = false;
	}
	if(render_counter != 1 ){
		render_counter = 1;
	}
}

void rend_set_fb_scale(float x, float y)
{
	renderer->SetFBScale(x, y);
}

bool RegisterRendererBackend(const rendererbackend_t& backend)
{
	if (!p_backends) {
		p_backends = new std::map<const string, rendererbackend_t>();
	}
	backends[backend.slug] = backend;
	return true;
}

vector<rendererbackend_t> rend_get_backends()
{
	vector<rendererbackend_t> vec;
	transform(backends.begin(), backends.end(), back_inserter(vec), [](const std::pair<string, rendererbackend_t>& x) { return x.second; });
	
	sort(vec.begin(), vec.end(), [](const rendererbackend_t& a, const rendererbackend_t& b) { return a.priority > b.priority; });

	return vec;
}
