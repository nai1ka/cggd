#include "dx12_renderer.h"

#include "utils/com_error_handler.h"
#include "utils/window.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <filesystem>


void cg::renderer::dx12_renderer::init()
{
	auto m = std::make_shared<cg::world::model>();
	m->load_obj(settings->model_path);
	model = m;

	auto cam = std::make_shared<cg::world::camera>();
	cam->set_height(static_cast<float>(settings->height));
	cam->set_width(static_cast<float>(settings->width));
	float3 pos = {settings->camera_position[0],
				  settings->camera_position[1],
				  settings->camera_position[2]};
	cam->set_position(pos);
	cam->set_theta(settings->camera_theta);
	cam->set_phi(settings->camera_phi);
	cam->set_angle_of_view(settings->camera_angle_of_view);
	cam->set_z_near(settings->camera_z_near);
	cam->set_z_far(settings->camera_z_far);
	camera = cam;

	auto viewWidth = static_cast<float>(settings->width);
	auto viewHeight = static_cast<float>(settings->height);
	view_port = CD3DX12_VIEWPORT(0.f, 0.f, viewWidth, viewHeight);
	scissor_rect = CD3DX12_RECT(0, 0, static_cast<LONG>(viewWidth), static_cast<LONG>(viewHeight));

	load_pipeline();
	load_assets();
}

void cg::renderer::dx12_renderer::destroy()
{
	wait_for_gpu();
	CloseHandle(fence_event);
}

void cg::renderer::dx12_renderer::update()
{
	auto now = std::chrono::high_resolution_clock::now();
	std::chrono::duration<float> duration = now - current_time;
	current_time = now;
	frame_duration = duration.count();

	cb.mwpMatrix = camera->get_dxm_mvp_matrix();
	memcpy(constant_buffer_data_begin, &cb, sizeof(cb));
}

void cg::renderer::dx12_renderer::render()
{
	populate_command_list();

	ID3D12CommandList* cmd_lists[] = {command_list.Get()};
	const UINT list_count = static_cast<UINT>(std::size(cmd_lists));
	command_queue->ExecuteCommandLists(list_count, cmd_lists);

	HRESULT presentation_result = swap_chain->Present(0, 0);

	THROW_IF_FAILED(presentation_result);
	move_to_next_frame();
}

ComPtr<IDXGIFactory4> cg::renderer::dx12_renderer::get_dxgi_factory()
{
	UINT factory_creation_flags = 0;

#ifdef _DEBUG
	ComPtr<ID3D12Debug> dbg_interface;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg_interface))))
	{
		dbg_interface->EnableDebugLayer();
		factory_creation_flags = DXGI_CREATE_FACTORY_DEBUG;
	}
#endif

	ComPtr<IDXGIFactory4> graphics_factory;
	HRESULT creation_result = CreateDXGIFactory2(factory_creation_flags, IID_PPV_ARGS(&graphics_factory));

	THROW_IF_FAILED(creation_result);
	return graphics_factory;
}

void cg::renderer::dx12_renderer::initialize_device(ComPtr<IDXGIFactory4>& dxgi_factory)
{
	ComPtr<IDXGIAdapter1> gpu_adapter;
	HRESULT adapter_result = dxgi_factory->EnumAdapters1(0, &gpu_adapter);

#ifdef _DEBUG
	if (SUCCEEDED(adapter_result))
	{
		DXGI_ADAPTER_DESC adapter_info{};
		if (SUCCEEDED(gpu_adapter->GetDesc(&adapter_info)))
		{
			OutputDebugString(adapter_info.Description);
			OutputDebugString(L"\n");
		}
	}
#endif

	const D3D_FEATURE_LEVEL required_level = D3D_FEATURE_LEVEL_11_0;
	HRESULT device_result = D3D12CreateDevice(
			gpu_adapter.Get(),
			required_level,
			IID_PPV_ARGS(&device));

	THROW_IF_FAILED(device_result);
}

void cg::renderer::dx12_renderer::create_direct_command_queue()
{
	D3D12_COMMAND_QUEUE_DESC cmd_queue_config{};
	cmd_queue_config.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	cmd_queue_config.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	HRESULT queue_creation_result = E_FAIL;

	if (device != nullptr)
	{
		queue_creation_result = device->CreateCommandQueue(
				&cmd_queue_config,
				IID_PPV_ARGS(&command_queue));
	}

	THROW_IF_FAILED(queue_creation_result);
}

void cg::renderer::dx12_renderer::create_swap_chain(ComPtr<IDXGIFactory4>& dxgi_factory)
{
	DXGI_SWAP_CHAIN_DESC1 sc_descriptor{};
	sc_descriptor.Width = settings->width;
	sc_descriptor.Height = settings->height;
	sc_descriptor.BufferCount = frame_number;
	sc_descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc_descriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc_descriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	sc_descriptor.SampleDesc = {1, 0};

	HWND window_handle = cg::utils::window::get_hwnd();
	ComPtr<IDXGISwapChain1> intermediate_chain;

	HRESULT create_result = dxgi_factory->CreateSwapChainForHwnd(
			command_queue.Get(),
			window_handle,
			&sc_descriptor,
			nullptr,
			nullptr,
			&intermediate_chain);

	THROW_IF_FAILED(create_result);

	dxgi_factory->MakeWindowAssociation(
			window_handle,
			DXGI_MWA_NO_ALT_ENTER);

	intermediate_chain.As(&swap_chain);
	frame_index = swap_chain->GetCurrentBackBufferIndex();
}


void cg::renderer::dx12_renderer::create_render_target_views()
{
	rtv_heap.create_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, frame_number);

	for (UINT buffer_idx = 0; buffer_idx < frame_number; buffer_idx++)
	{
		HRESULT buffer_result = swap_chain->GetBuffer(
				buffer_idx,
				IID_PPV_ARGS(&render_targets[buffer_idx]));

		THROW_IF_FAILED(buffer_result);

		std::wstring target_name = L"Render target " + std::to_wstring(buffer_idx);
		render_targets[buffer_idx]->SetName(target_name.c_str());

		auto descriptor_handle = rtv_heap.get_cpu_descriptor_handle(buffer_idx);
		device->CreateRenderTargetView(
				render_targets[buffer_idx].Get(),
				nullptr,
				descriptor_handle);
	}
}

void cg::renderer::dx12_renderer::create_depth_buffer()
{
}

void cg::renderer::dx12_renderer::create_command_allocators()
{
	const D3D12_COMMAND_LIST_TYPE alloc_type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	for (auto& command_allocator: command_allocators)
	{
		HRESULT alloc_result = device->CreateCommandAllocator(
				alloc_type,
				IID_PPV_ARGS(&command_allocator));

		THROW_IF_FAILED(alloc_result);
	}
}

void cg::renderer::dx12_renderer::create_command_list()
{
	const UINT node_mask = 0;
	const D3D12_COMMAND_LIST_TYPE list_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ID3D12CommandAllocator* initial_allocator = command_allocators[0].Get();
	ID3D12PipelineState* initial_state = pipeline_state.Get();

	HRESULT list_creation_result = device->CreateCommandList(
			node_mask,
			list_type,
			initial_allocator,
			initial_state,
			IID_PPV_ARGS(&command_list));

	THROW_IF_FAILED(list_creation_result);
}

void cg::renderer::dx12_renderer::load_pipeline()
{
	ComPtr<IDXGIFactory4> dxgi_factory = get_dxgi_factory();
	initialize_device(dxgi_factory);
	create_direct_command_queue();
	create_swap_chain(dxgi_factory);
	create_render_target_views();
}

D3D12_STATIC_SAMPLER_DESC cg::renderer::dx12_renderer::get_sampler_descriptor()
{
	D3D12_STATIC_SAMPLER_DESC sampler_desc{};
	return sampler_desc;
}

void cg::renderer::dx12_renderer::create_root_signature(const D3D12_STATIC_SAMPLER_DESC* sampler_descriptors, UINT num_sampler_descriptors)
{
	CD3DX12_ROOT_PARAMETER1 signature_params[1];
	CD3DX12_DESCRIPTOR_RANGE1 descriptor_ranges[1];

	descriptor_ranges[0].Init(
			D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
			1,// descriptor count
			0,// register
			0,// space
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

	signature_params[0].InitAsDescriptorTable(
			1,
			&descriptor_ranges[0],
			D3D12_SHADER_VISIBILITY_ALL);

	D3D12_FEATURE_DATA_ROOT_SIGNATURE feature_support{};
	feature_support.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

	bool supports_v1_1 = true;
	if (device->CheckFeatureSupport(
				D3D12_FEATURE_ROOT_SIGNATURE,
				&feature_support,
				sizeof(feature_support)) != S_OK)
	{
		supports_v1_1 = false;
		feature_support.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

	D3D12_ROOT_SIGNATURE_FLAGS signature_flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_definition;
	rs_definition.Init_1_1(
			static_cast<UINT>(std::size(signature_params)),
			signature_params,
			num_sampler_descriptors,
			sampler_descriptors,
			signature_flags);

	ComPtr<ID3DBlob> serialized_signature;
	ComPtr<ID3DBlob> error_blob;

	HRESULT serialize_result = D3DX12SerializeVersionedRootSignature(
			&rs_definition,
			feature_support.HighestVersion,
			&serialized_signature,
			&error_blob);


	if (error_blob)
	{
		OutputDebugStringA(static_cast<char*>(error_blob->GetBufferPointer()));
	}
	THROW_IF_FAILED(serialize_result);

	HRESULT creation_result = device->CreateRootSignature(
			0,// nodeMask
			serialized_signature->GetBufferPointer(),
			serialized_signature->GetBufferSize(),
			IID_PPV_ARGS(&root_signature));


	THROW_IF_FAILED(creation_result);
}

std::filesystem::path cg::renderer::dx12_renderer::get_shader_path()
{
	WCHAR module_path[MAX_PATH];
	ZeroMemory(module_path, sizeof(module_path));

	if (GetModuleFileName(nullptr, module_path, MAX_PATH) == 0)
	{
		return {};
	}

	auto executable_dir = std::filesystem::path(module_path).parent_path();
	auto hlsl_file_path = executable_dir / "shaders.hlsl";

	if (std::filesystem::exists(hlsl_file_path))
	{
		std::cout << "Shader path: " << hlsl_file_path.string() << std::endl;
	}
	else
	{
		std::cout << "Shader file not found at expected location" << std::endl;
	}

	return hlsl_file_path;
}

ComPtr<ID3DBlob> cg::renderer::dx12_renderer::compile_shader(const std::string& entrypoint, const std::string& target)
{
	ComPtr<ID3DBlob> compiled_code = nullptr;
	ComPtr<ID3DBlob> compilation_errors = nullptr;

	UINT shader_flags = 0;

#ifdef _DEBUG
	shader_flags = D3DCOMPILE_DEBUG;
	shader_flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	auto shader_file_path = get_shader_path();
	if (!std::filesystem::exists(shader_file_path))
	{
		THROW_IF_FAILED(E_FAIL);
		return nullptr;
	}

	HRESULT compilation_result = D3DCompileFromFile(
			shader_file_path.wstring().c_str(),
			nullptr,// defines
			nullptr,// include handler
			entrypoint.c_str(),
			target.c_str(),
			shader_flags,
			0,// effect flags
			&compiled_code,
			&compilation_errors);

	if (compilation_errors)
	{
		const char* error_message = static_cast<const char*>(
				compilation_errors->GetBufferPointer());
		OutputDebugStringA(error_message);
	}

	THROW_IF_FAILED(compilation_result);

	return compiled_code;
}


void cg::renderer::dx12_renderer::create_pso()
{
	ComPtr<ID3DBlob> vs_bytecode = compile_shader("VSMain", "vs_5_0");
	ComPtr<ID3DBlob> ps_bytecode = compile_shader("PSMain", "ps_5_0");

	D3D12_INPUT_ELEMENT_DESC vertex_layout[] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
			 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
			 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
			 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			{"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32,
			 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			{"COLOR", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44,
			 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			{"COLOR", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 56,
			 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_config{};

	pipeline_config.pRootSignature = root_signature.Get();

	pipeline_config.VS.pShaderBytecode = vs_bytecode->GetBufferPointer();
	pipeline_config.VS.BytecodeLength = vs_bytecode->GetBufferSize();

	pipeline_config.PS.pShaderBytecode = ps_bytecode->GetBufferPointer();
	pipeline_config.PS.BytecodeLength = ps_bytecode->GetBufferSize();

	pipeline_config.InputLayout.pInputElementDescs = vertex_layout;
	pipeline_config.InputLayout.NumElements = static_cast<UINT>(std::size(vertex_layout));

	pipeline_config.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pipeline_config.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pipeline_config.RasterizerState.FrontCounterClockwise = TRUE;

	pipeline_config.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

	pipeline_config.DepthStencilState.DepthEnable = FALSE;
	pipeline_config.DepthStencilState.StencilEnable = FALSE;

	pipeline_config.SampleMask = UINT_MAX;
	pipeline_config.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipeline_config.NumRenderTargets = 1;
	pipeline_config.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pipeline_config.SampleDesc = {1, 0};

	HRESULT creation_result = device->CreateGraphicsPipelineState(
			&pipeline_config,
			IID_PPV_ARGS(&pipeline_state));

	THROW_IF_FAILED(creation_result);
}

void cg::renderer::dx12_renderer::create_resource_on_upload_heap(ComPtr<ID3D12Resource>& resource, UINT size, const std::wstring& name)
{
	const D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_UPLOAD;
	const D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE;
	const D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_GENERIC_READ;

	auto heap_props = CD3DX12_HEAP_PROPERTIES(heap_type);
	auto buffer_desc = CD3DX12_RESOURCE_DESC::Buffer(size);

	HRESULT allocation_result = device->CreateCommittedResource(
			&heap_props,
			heap_flags,
			&buffer_desc,
			initial_state,
			nullptr,// clear value
			IID_PPV_ARGS(&resource));

	if (SUCCEEDED(allocation_result))
	{
		if (name.length() > 0)
		{
			resource->SetName(name.c_str());
		}
	}
	else
	{
		THROW_IF_FAILED(allocation_result);
	}
}

void cg::renderer::dx12_renderer::create_resource_on_default_heap(ComPtr<ID3D12Resource>& resource, UINT size, const std::wstring& name, D3D12_RESOURCE_DESC* resource_descriptor)
{
}

void cg::renderer::dx12_renderer::copy_data(const void* buffer_data, UINT buffer_size, ComPtr<ID3D12Resource>& destination_resource)
{
	UINT8* buffer_data_begin;
	CD3DX12_RANGE read_range(0, 0);
	THROW_IF_FAILED(
			destination_resource->Map(0, &read_range,
									  reinterpret_cast<void**>(&buffer_data_begin)));
	memcpy(buffer_data_begin, buffer_data, buffer_size);
	destination_resource->Unmap(0, 0);
}

void cg::renderer::dx12_renderer::copy_data(const void* buffer_data, const UINT buffer_size, ComPtr<ID3D12Resource>& destination_resource, ComPtr<ID3D12Resource>& intermediate_resource, D3D12_RESOURCE_STATES state_after, int row_pitch, int slice_pitch)
{
}

D3D12_VERTEX_BUFFER_VIEW cg::renderer::dx12_renderer::create_vertex_buffer_view(const ComPtr<ID3D12Resource>& vertex_buffer, const UINT vertex_buffer_size)
{
	D3D12_VERTEX_BUFFER_VIEW buffer_descriptor{};

	if (vertex_buffer && vertex_buffer_size > 0)
	{
		buffer_descriptor.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
		buffer_descriptor.SizeInBytes = vertex_buffer_size;
		buffer_descriptor.StrideInBytes = static_cast<UINT>(sizeof(vertex));
	}

	return buffer_descriptor;
}

D3D12_INDEX_BUFFER_VIEW cg::renderer::dx12_renderer::create_index_buffer_view(const ComPtr<ID3D12Resource>& index_buffer, const UINT index_buffer_size)
{
	D3D12_INDEX_BUFFER_VIEW idx_view{};


	idx_view.BufferLocation = index_buffer->GetGPUVirtualAddress();
	idx_view.SizeInBytes = index_buffer_size;
	idx_view.Format = DXGI_FORMAT_R32_UINT;

	return idx_view;
}

void cg::renderer::dx12_renderer::create_shader_resource_view(const ComPtr<ID3D12Resource>& texture, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handler)
{
}

void cg::renderer::dx12_renderer::create_constant_buffer_view(const ComPtr<ID3D12Resource>& buffer, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handler)
{
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_descriptor{};

	cbv_descriptor.BufferLocation = buffer->GetGPUVirtualAddress();

	const UINT cb_size = sizeof(cb);
	const UINT aligned_size = (cb_size + 255) & ~255;
	cbv_descriptor.SizeInBytes = aligned_size;

	device->CreateConstantBufferView(
			&cbv_descriptor,
			cpu_handler);
}

void cg::renderer::dx12_renderer::load_assets()
{
	create_root_signature(nullptr, 0);
	create_pso();
	create_command_allocators();
	create_command_list();

	const size_t buffer_count = model->get_vertex_buffers().size();
	vertex_buffers.resize(buffer_count);
	vertex_buffer_views.resize(buffer_count);
	index_buffers.resize(buffer_count);
	index_buffer_views.resize(buffer_count);

	for (size_t mesh_idx = 0; mesh_idx < buffer_count; mesh_idx++)
	{
		auto vb_source = model->get_vertex_buffers()[mesh_idx];
		const UINT vb_size = static_cast<UINT>(vb_source->get_size_in_bytes());

		std::wstring vb_label = L"Vertex buffer " + std::to_wstring(mesh_idx);

		create_resource_on_upload_heap(
				vertex_buffers[mesh_idx],
				vb_size,
				vb_label);

		if (vb_size > 0)
		{
			copy_data(
					vb_source->get_data(),
					vb_size,
					vertex_buffers[mesh_idx]);

			vertex_buffer_views[mesh_idx] = create_vertex_buffer_view(
					vertex_buffers[mesh_idx],
					vb_size);
		}


		auto ib_source = model->get_index_buffers()[mesh_idx];
		const UINT ib_size = static_cast<UINT>(ib_source->get_size_in_bytes());

		std::wstring ib_label = L"Index buffer " + std::to_wstring(mesh_idx);

		create_resource_on_upload_heap(
				index_buffers[mesh_idx],
				ib_size,
				ib_label);

		if (ib_size > 0)
		{
			copy_data(
					ib_source->get_data(),
					ib_size,
					index_buffers[mesh_idx]);

			index_buffer_views[mesh_idx] = create_index_buffer_view(
					index_buffers[mesh_idx],
					ib_size);
		}
	}

	const UINT cb_reserve_size = 64 * 1024;
	create_resource_on_upload_heap(
			constant_buffer,
			cb_reserve_size,
			L"Constant buffer");

	copy_data(&cb, sizeof(cb), constant_buffer);

	CD3DX12_RANGE no_read_range(0, 0);
	HRESULT map_result = constant_buffer->Map(
			0,
			&no_read_range,
			reinterpret_cast<void**>(&constant_buffer_data_begin));

	THROW_IF_FAILED(map_result);

	cbv_srv_heap.create_heap(
			device,
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			1,
			D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

	create_constant_buffer_view(
			constant_buffer,
			cbv_srv_heap.get_cpu_descriptor_handle(0));

	HRESULT close_result = command_list->Close();

	THROW_IF_FAILED(close_result);

	HRESULT fence_result = device->CreateFence(
			0,
			D3D12_FENCE_FLAG_NONE,
			IID_PPV_ARGS(&fence));

	THROW_IF_FAILED(fence_result);

	fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!fence_event)
	{
		THROW_IF_FAILED(HRESULT_FROM_WIN32(GetLastError()));
	}

	wait_for_gpu();
}


void cg::renderer::dx12_renderer::populate_command_list()
{
	HRESULT allocator_reset = command_allocators[frame_index]->Reset();

	THROW_IF_FAILED(allocator_reset);

	HRESULT list_reset = command_list->Reset(
			command_allocators[frame_index].Get(),
			pipeline_state.Get());

	THROW_IF_FAILED(list_reset);


	command_list->SetGraphicsRootSignature(root_signature.Get());



	ID3D12DescriptorHeap* active_heaps[] = {cbv_srv_heap.get()};
	command_list->SetDescriptorHeaps(1, active_heaps);
	command_list->SetGraphicsRootDescriptorTable(0, cbv_srv_heap.get_gpu_descriptor_handle(0));

	command_list->RSSetViewports(1, &view_port);
	command_list->RSSetScissorRects(1, &scissor_rect);

	auto transition_to_render = CD3DX12_RESOURCE_BARRIER::Transition(
			render_targets[frame_index].Get(),
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
	command_list->ResourceBarrier(1, &transition_to_render);

	auto rtv_handle = rtv_heap.get_cpu_descriptor_handle(frame_index);
	command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

	float background_color[] = {0.47f, 0.69f, 0.811f, 1.0f};
	command_list->ClearRenderTargetView(rtv_handle, background_color, 0, nullptr);

	command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const size_t mesh_count = model->get_index_buffers().size();
	for (size_t mesh_idx = 0; mesh_idx < mesh_count; mesh_idx++)
	{
		command_list->IASetVertexBuffers(0, 1, &vertex_buffer_views[mesh_idx]);
		command_list->IASetIndexBuffer(&index_buffer_views[mesh_idx]);

		UINT index_count = static_cast<UINT>(
				model->get_index_buffers()[mesh_idx]->get_number_of_elements());

		command_list->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
	}

	auto transition_to_present = CD3DX12_RESOURCE_BARRIER::Transition(
			render_targets[frame_index].Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PRESENT);
	command_list->ResourceBarrier(1, &transition_to_present);

	HRESULT close_result = command_list->Close();

	THROW_IF_FAILED(close_result);
}


void cg::renderer::dx12_renderer::move_to_next_frame()
{
	const UINT64 fence_value_for_signal = fence_values[frame_index];

	HRESULT signal_result = command_queue->Signal(
			fence.Get(),
			fence_value_for_signal);


	THROW_IF_FAILED(signal_result);

	frame_index = swap_chain->GetCurrentBackBufferIndex();

	if (fence->GetCompletedValue() < fence_values[frame_index])
	{
		HRESULT event_result = fence->SetEventOnCompletion(
				fence_values[frame_index],
				fence_event);


		THROW_IF_FAILED(event_result);
		WaitForSingleObjectEx(fence_event, INFINITE, FALSE);
	}

	fence_values[frame_index] = fence_value_for_signal + 1;
}

void cg::renderer::dx12_renderer::wait_for_gpu()
{
	THROW_IF_FAILED(command_queue->Signal(fence.Get(), fence_values[frame_index]));
	THROW_IF_FAILED(fence->SetEventOnCompletion(
			fence_values[frame_index],
			fence_event));

	WaitForSingleObjectEx(fence_event, INFINITE, FALSE);
	fence_values[frame_index]++;
}


void cg::renderer::descriptor_heap::create_heap(ComPtr<ID3D12Device>& device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT number, D3D12_DESCRIPTOR_HEAP_FLAGS flags)
{

	D3D12_DESCRIPTOR_HEAP_DESC heap_configuration{};
	heap_configuration.Type = type;
	heap_configuration.NumDescriptors = number;
	heap_configuration.Flags = flags;
	heap_configuration.NodeMask = 0;

	HRESULT creation_status = device->CreateDescriptorHeap(
			&heap_configuration,
			IID_PPV_ARGS(&heap));

	THROW_IF_FAILED(creation_status);
	descriptor_size = device->GetDescriptorHandleIncrementSize(type);
}

D3D12_CPU_DESCRIPTOR_HANDLE cg::renderer::descriptor_heap::get_cpu_descriptor_handle(UINT index) const
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
			heap->GetCPUDescriptorHandleForHeapStart(),
			static_cast<INT>(index),
			descriptor_size);
}

D3D12_GPU_DESCRIPTOR_HANDLE cg::renderer::descriptor_heap::get_gpu_descriptor_handle(UINT index) const
{
	return CD3DX12_GPU_DESCRIPTOR_HANDLE(
			heap->GetGPUDescriptorHandleForHeapStart(),
			static_cast<INT>(index),
			descriptor_size);
}
ID3D12DescriptorHeap* cg::renderer::descriptor_heap::get() const
{
	return heap.Get();
}