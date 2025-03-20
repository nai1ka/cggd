#pragma once

#include "resource.h"

#include <iostream>
#include <linalg.h>
#include <memory>
#include <omp.h>
#include <random>

using namespace linalg::aliases;

namespace cg::renderer
{
	struct ray
	{
		ray(float3 position, float3 direction) : position(position)
		{
			this->direction = normalize(direction);
		}
		float3 position;
		float3 direction;
	};

	struct payload
	{
		float t;
		float3 bary;
		cg::color color;
	};

	template<typename VB>
	struct triangle
	{
		triangle(const VB& vertex_a, const VB& vertex_b, const VB& vertex_c);

		float3 a;
		float3 b;
		float3 c;

		float3 ba;
		float3 ca;

		float3 na;
		float3 nb;
		float3 nc;

		float3 ambient;
		float3 diffuse;
		float3 emissive;
	};

	template<typename VB>
	inline triangle<VB>::triangle(const VB& vertex_a, const VB& vertex_b,
								  const VB& vertex_c)
	{
		a = float3{vertex_a.x, vertex_a.y, vertex_a.z};
		b = float3{vertex_b.x, vertex_b.y, vertex_b.z};
		c = float3{vertex_c.x, vertex_c.y, vertex_c.z};

		ba = b - a;
		ca = c - a;

		na = float3{vertex_a.nx, vertex_a.ny, vertex_a.nz};
		nb = float3{vertex_b.nx, vertex_b.ny, vertex_b.nz};
		nc = float3{vertex_c.nx, vertex_c.ny, vertex_c.nz};

		ambient = float3{vertex_a.ambient_r, vertex_a.ambient_g, vertex_a.ambient_b};
		diffuse = float3{vertex_a.diffuse_r, vertex_a.diffuse_g, vertex_a.diffuse_b};
		emissive =
				float3{vertex_a.emissive_r, vertex_a.emissive_g, vertex_a.emissive_b};
	}

	template<typename VB>
	class aabb
	{
	public:
		void add_triangle(const triangle<VB> triangle);
		const std::vector<triangle<VB>>& get_triangles() const;
		bool aabb_test(const ray& ray) const;

	protected:
		std::vector<triangle<VB>> triangles;

		float3 aabb_min;
		float3 aabb_max;
	};

	struct light
	{
		float3 position;
		float3 color;
	};

	template<typename VB, typename RT>
	class raytracer
	{
	public:
		raytracer() {};
		~raytracer() {};

		void set_render_target(std::shared_ptr<resource<RT>> in_render_target);
		void clear_render_target(const RT& in_clear_value);
		void set_viewport(size_t in_width, size_t in_height);

		void set_vertex_buffers(
				std::vector<std::shared_ptr<cg::resource<VB>>> in_vertex_buffers);
		void
		set_index_buffers(std::vector<std::shared_ptr<cg::resource<unsigned int>>>
								  in_index_buffers);
		void build_acceleration_structure();
		std::vector<aabb<VB>> acceleration_structures;

		void ray_generation(float3 position, float3 direction, float3 right,
							float3 up, size_t depth, size_t accumulation_num);

		payload trace_ray(const ray& ray, size_t depth, float max_t = 1000.f,
						  float min_t = 0.001f) const;
		payload intersection_shader(const triangle<VB>& triangle,
									const ray& ray) const;

		std::function<payload(const ray& ray)> miss_shader = nullptr;
		std::function<payload(const ray& ray, payload& payload,
							  const triangle<VB>& triangle, size_t depth)>
				closest_hit_shader = nullptr;
		std::function<payload(const ray& ray, payload& payload,
							  const triangle<VB>& triangle)>
				any_hit_shader = nullptr;

		float2 get_jitter(int frame_id);

	protected:
		std::shared_ptr<cg::resource<RT>> render_target;
		std::shared_ptr<cg::resource<float3>> history;
		std::vector<std::shared_ptr<cg::resource<unsigned int>>> index_buffers;
		std::vector<std::shared_ptr<cg::resource<VB>>> vertex_buffers;
		std::vector<triangle<VB>> triangles;

		size_t width = 1920;
		size_t height = 1080;
	};

	template<typename VB, typename RT>
	inline void raytracer<VB, RT>::set_render_target(
			std::shared_ptr<resource<RT>> in_render_target)
	{
		render_target = in_render_target;
	}

	template<typename VB, typename RT>
	inline void raytracer<VB, RT>::set_viewport(size_t in_width, size_t in_height)
	{
		height = in_height;
		width = in_width;

		history = std::make_shared<cg::resource<float3>>(width, height);
	}

	template<typename VB, typename RT>
	inline void raytracer<VB, RT>::clear_render_target(const RT& in_clear_value)
	{
		for (size_t i = 0; i < render_target->get_number_of_elements(); i++) {
			render_target->item(i) = in_clear_value;
			history->item(i) = float3{0.f, 0.f, 0.f};
		}
	}

	template<typename VB, typename RT>
	inline void raytracer<VB, RT>::set_vertex_buffers(
			std::vector<std::shared_ptr<cg::resource<VB>>> in_vertex_buffers)
	{
		vertex_buffers = in_vertex_buffers;
	}

	template<typename VB, typename RT>
	void raytracer<VB, RT>::set_index_buffers(
			std::vector<std::shared_ptr<cg::resource<unsigned int>>> in_index_buffers)
	{
		index_buffers = in_index_buffers;
	}

	template<typename VB, typename RT>
	inline void raytracer<VB, RT>::build_acceleration_structure()
	{
		acceleration_structures.clear();
		acceleration_structures.reserve(index_buffers.size());

		for (size_t shape_id = 0; shape_id < index_buffers.size(); shape_id++) {
			auto& indices = index_buffers[shape_id];
			auto& vertices = vertex_buffers[shape_id];

			aabb<VB> bounding_box;

			const size_t triangle_count = indices->get_number_of_elements() / 3;

			for (size_t tri_idx = 0; tri_idx < triangle_count; tri_idx++) {
				const size_t base_idx = tri_idx * 3;

				const auto& vertex_a = vertices->item(indices->item(base_idx));
				const auto& vertex_b = vertices->item(indices->item(base_idx + 1));
				const auto& vertex_c = vertices->item(indices->item(base_idx + 2));

				triangle<VB> current_triangle(vertex_a, vertex_b, vertex_c);
				bounding_box.add_triangle(current_triangle);
			}

			acceleration_structures.push_back(bounding_box);
		}
	}
	template<typename VB, typename RT>
	inline void raytracer<VB, RT>::ray_generation(float3 position, float3 direction,
												  float3 right, float3 up,
												  size_t depth,
												  size_t accumulation_num)
	{
		float inv_accum = 1.f / static_cast<float>(accumulation_num);

		for (int frame = 0; frame < accumulation_num; frame++) {
			std::cout << "Tracing frame #" << frame + 1 << "\n";
			float2 jitter = get_jitter(frame);

#pragma omp parallel for
			for (int x = 0; x < width; x++) {
				for (int y = 0; y < height; y++) {
					float u = (2.f * x + jitter.x) / static_cast<float>(width - 1) - 1.f;
					float v = (2.f * y + jitter.y) / static_cast<float>(height - 1) - 1.f;
					u *= static_cast<float>(width) / static_cast<float>(height);

					float3 ray_dir = direction + u * right - v * up;
					ray current_ray(position, ray_dir);

					payload hit_result = trace_ray(current_ray, depth);

					auto& pixel_history = history->item(x, y);
					pixel_history += sqrt(hit_result.color.to_float3() * inv_accum);

					if (frame == accumulation_num - 1)
						render_target->item(x, y) = RT::from_float3(pixel_history);
				}
			}
		}
	}

	template<typename VB, typename RT>
	inline payload raytracer<VB, RT>::trace_ray(const ray& ray, size_t depth,
												float max_t, float min_t) const
	{
		if (depth == 0)
			return miss_shader(ray);

		size_t next_depth = depth - 1;

		payload best_hit{};
		best_hit.t = max_t;
		const triangle<VB>* hit_triangle = nullptr;

		for (auto& bounding_box: acceleration_structures) {
			if (!bounding_box.aabb_test(ray))
				continue;

			for (auto& tri: bounding_box.get_triangles()) {
				payload current_hit = intersection_shader(tri, ray);

				if (current_hit.t > min_t && current_hit.t < best_hit.t) {
					best_hit = current_hit;
					hit_triangle = &tri;

					if (any_hit_shader)
						return any_hit_shader(ray, current_hit, tri);
				}
			}
		}

		if (hit_triangle && closest_hit_shader)
			return closest_hit_shader(ray, best_hit, *hit_triangle, next_depth);

		return miss_shader(ray);
	}

	template<typename VB, typename RT>
	inline payload
	raytracer<VB, RT>::intersection_shader(const triangle<VB>& triangle,
										   const ray& ray) const
	{
		payload result{};
		result.t = -1.f;

		float3 edge1 = triangle.ba;
		float3 edge2 = triangle.ca;
		float3 h = cross(ray.direction, edge2);

		float determinant = dot(edge1, h);
		if (determinant > -1e-8 && determinant < 1e-8)
			return result;

		float inv_determinant = 1.f / determinant;
		float3 s = ray.position - triangle.a;

		float u_coord = dot(s, h) * inv_determinant;
		if (u_coord < 0.f || u_coord > 1.f)
			return result;

		float3 q = cross(s, edge1);
		float v_coord = dot(ray.direction, q) * inv_determinant;
		if (v_coord < 0.f || u_coord + v_coord > 1.f)
			return result;

		result.t = dot(edge2, q) * inv_determinant;
		result.bary = float3{1.f - u_coord - v_coord, u_coord, v_coord};

		return result;
	}

	template<typename VB, typename RT>
	float2 raytracer<VB, RT>::get_jitter(int frame_id)
	{
		float2 result{0.f, 0.f};

		int adjusted_id = frame_id + 1;

		constexpr int x_base = 2;
		float x_inv_base = 1.f / static_cast<float>(x_base);
		float x_fraction = x_inv_base;

		int x_index = adjusted_id;
		while (x_index > 0) {
			result.x += (x_index % x_base) * x_fraction;
			x_index /= x_base;
			x_fraction *= x_inv_base;
		}

		constexpr int y_base = 3;
		float y_inv_base = 1.f / static_cast<float>(y_base);
		float y_fraction = y_inv_base;

		int y_index = adjusted_id;
		while (y_index > 0) {
			result.y += (y_index % y_base) * y_fraction;
			y_index /= y_base;
			y_fraction *= y_inv_base;
		}

		return result - 0.5f;
	}

	template<typename VB>
	inline void aabb<VB>::add_triangle(const triangle<VB> triangle)
	{
		if (triangles.empty()) {
			aabb_max = aabb_min = triangle.a;
		}

		triangles.push_back(triangle);

		aabb_max = max(aabb_max, triangle.a);
		aabb_max = max(aabb_max, triangle.b);
		aabb_max = max(aabb_max, triangle.c);

		aabb_min = min(aabb_min, triangle.a);
		aabb_min = min(aabb_min, triangle.b);
		aabb_min = min(aabb_min, triangle.c);
	}

	template<typename VB>
	inline const std::vector<triangle<VB>>& aabb<VB>::get_triangles() const
	{
		return triangles;
	}

	template<typename VB>
	inline bool aabb<VB>::aabb_test(const ray& ray) const
	{
		float3 reciprocal_dir = 1.f / ray.direction;
		float3 t_far = (aabb_max - ray.position) * reciprocal_dir;
		float3 t_near = (aabb_min - ray.position) * reciprocal_dir;
		float3 t_min = min(t_near, t_far);
		float3 t_max = max(t_near, t_far);
		return maxelem(t_min) <= minelem(t_max);
	}

}// namespace cg::renderer