#include "raytracer_renderer.h"

#include "utils/resource_utils.h"

#include <iostream>


void cg::renderer::ray_tracing_renderer::init_raytracer()
{
  raytracer = std::make_shared<
      cg::renderer::raytracer<cg::vertex, cg::unsigned_color>>();
  raytracer->set_viewport(settings->width, settings->height);

  render_target = std::make_shared<cg::resource<cg::unsigned_color>>(
      settings->width, settings->height);

  raytracer->set_render_target(render_target);
}

void cg::renderer::ray_tracing_renderer::init_model()
{
  model = std::make_shared<cg::world::model>();
  model->load_obj(settings->model_path);

  raytracer->set_vertex_buffers(model->get_vertex_buffers());
  raytracer->set_index_buffers(model->get_index_buffers());
  
  shadow_raytracer->set_vertex_buffers(model->get_vertex_buffers());
  shadow_raytracer->set_index_buffers(model->get_index_buffers());
}

void cg::renderer::ray_tracing_renderer::init_camera()
{
  camera = std::make_shared<cg::world::camera>();
  camera->set_height(static_cast<float>(settings->height));
  camera->set_width(static_cast<float>(settings->width));
  camera->set_position(float3{
      settings->camera_position[0],
      settings->camera_position[1],
      settings->camera_position[2],
  });
  camera->set_phi(settings->camera_phi);
  camera->set_theta(settings->camera_theta);
  camera->set_angle_of_view(settings->camera_angle_of_view);
  camera->set_z_near(settings->camera_z_near);
  camera->set_z_far(settings->camera_z_far);
}

void cg::renderer::ray_tracing_renderer::init_lights()
{
  lights.push_back({float3{0.f, 1.58f, -0.03f}, float3{0.78f, 0.78f, 0.78f}});
}

void cg::renderer::ray_tracing_renderer::init_shadow_raytracer()
{
  shadow_raytracer = std::make_shared<
      cg::renderer::raytracer<cg::vertex, cg::unsigned_color>>();
}

void cg::renderer::ray_tracing_renderer::init()
{
  init_raytracer();
  init_shadow_raytracer();
  init_model();
  init_camera();
  init_lights();
}

void cg::renderer::ray_tracing_renderer::destroy() {}

void cg::renderer::ray_tracing_renderer::update() {}
void cg::renderer::ray_tracing_renderer::setup_shadow_raytracer()
{
    shadow_raytracer->miss_shader = [](const ray &ray) {
        payload payload{};
        payload.t = -1.f;
        return payload;
    };

    shadow_raytracer->any_hit_shader = [](const ray &ray, payload &payload,
                                      const triangle<cg::vertex> &triangle) {
        return payload;
    };
    shadow_raytracer->build_acceleration_structure();
}

void cg::renderer::ray_tracing_renderer::setup_main_raytracer()
{
    raytracer->clear_render_target({0, 0, 0});
    raytracer->miss_shader = [](const ray &ray) {
        payload payload{};
        payload.color = {0.f, 0.f, 0.f};
        return payload;
    };
    
    raytracer->acceleration_structures = shadow_raytracer->acceleration_structures;
}

std::tuple<std::mt19937, std::uniform_real_distribution<float>> 
cg::renderer::ray_tracing_renderer::create_random_generator()
{
    std::random_device random_device;
    std::mt19937 generator(random_device());
    std::uniform_real_distribution<float> distribution(-1.f, +1.f);
    
    return {generator, distribution};
}

void cg::renderer::ray_tracing_renderer::setup_closest_hit_shader(
    std::mt19937& random_generator,
    std::uniform_real_distribution<float>& uniform_distribution)
{
    raytracer->closest_hit_shader = [&](const ray &ray, payload &payload,
                                    const triangle<cg::vertex> &triangle,
                                    size_t depth) {
        float3 hit_position = ray.position + ray.direction * payload.t;
        float3 surface_normal = normalize(
            payload.bary.x * triangle.na + 
            payload.bary.y * triangle.nb +
            payload.bary.z * triangle.nc
        );

        float3 result_color = triangle.emissive;

        float3 random_direction{
            uniform_distribution(random_generator),
            uniform_distribution(random_generator),
            uniform_distribution(random_generator),
        };
        
        if (dot(surface_normal, random_direction) < 0.f) {
            random_direction = -random_direction;
        }

        cg::renderer::ray next_ray(hit_position, random_direction);
        auto next_payload = raytracer->trace_ray(next_ray, depth);

        result_color += triangle.diffuse * next_payload.color.to_float3() *
                      std::max(dot(surface_normal, next_ray.direction), 0.f);
                      
        payload.color = cg::color::from_float3(result_color);
        return payload;
    };
}

void cg::renderer::ray_tracing_renderer::trace_rays_and_save()
{
    auto start = std::chrono::high_resolution_clock::now();
    
    raytracer->ray_generation(
        camera->get_position(), 
        camera->get_direction(), 
        camera->get_right(),
        camera->get_up(), 
        settings->raytracing_depth, 
        settings->accumulation_num
    );

    auto stop = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, std::milli> duration = stop - start;
    std::cout << "Raytracing time: " << duration.count() << "ms\n";

    cg::utils::save_resource(*render_target, settings->result_path);
}

void cg::renderer::ray_tracing_renderer::render()
{
    setup_shadow_raytracer();
    setup_main_raytracer();
    
    auto [random_generator, uniform_distribution] = create_random_generator();
    setup_closest_hit_shader(random_generator, uniform_distribution);
    
    trace_rays_and_save();
}