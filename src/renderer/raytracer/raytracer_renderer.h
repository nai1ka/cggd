#include "renderer/raytracer/raytracer.h"
#include "renderer/renderer.h"
#include "resource.h"


namespace cg::renderer
{
	class ray_tracing_renderer : public renderer
	{
	public:
		virtual void init();
		virtual void destroy();

		virtual void update();
		virtual void render();

	protected:
		std::shared_ptr<cg::resource<cg::unsigned_color>> render_target;

		std::shared_ptr<cg::renderer::raytracer<cg::vertex, cg::unsigned_color>> raytracer;
		std::shared_ptr<cg::renderer::raytracer<cg::vertex, cg::unsigned_color>> shadow_raytracer;

		std::vector<cg::renderer::light> lights;

	private:
		void init_raytracer();
		void init_model();
		void init_camera();
		void init_lights();
		void init_shadow_raytracer();
		void setup_shadow_raytracer();
		void setup_main_raytracer();
		std::tuple<std::mt19937, std::uniform_real_distribution<float>> create_random_generator();
		void setup_closest_hit_shader(std::mt19937& random_generator, 
								   std::uniform_real_distribution<float>& uniform_distribution);
		void trace_rays_and_save();
	};
}// namespace cg::renderer