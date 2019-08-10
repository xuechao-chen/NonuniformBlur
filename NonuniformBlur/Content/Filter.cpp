#include "stdafx.h"
#include "Filter.h"
#include "Advanced/XUSGDDSLoader.h"

#define SizeOfInUint32(obj)	DIV_UP(sizeof(obj), sizeof(uint32_t))

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct GaussianConstants
{
	XMFLOAT2	Focus;
	float		Sigma;
	uint32_t	Level;
};

Filter::Filter(const Device& device) :
	m_device(device),
	m_numMips(11)
{
	m_computePipelineCache.SetDevice(device);
	m_descriptorTableCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

Filter::~Filter()
{
}

bool Filter::Init(const CommandList& commandList, uint32_t width, uint32_t height,
	DescriptorTable& uavSrvTable, shared_ptr<ResourceBase>& source,
	vector<Resource>& uploaders, Format rtFormat, const wchar_t* fileName)
{
	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.push_back(nullptr);
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, commandList, fileName,
			8192, false, source, uploaders.back(), &alphaMode), false);
	}

	// Create resources and pipelines
	m_numMips = (max)(Log2((max)(width, height)), 0ui8) + 1;

	for (auto& image : m_filtered)
		image.Create(m_device, width, height, rtFormat, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_numMips);

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(), false);
	N_RETURN(createDescriptorTables(), false);

	// Copy source
	{
		const Descriptor descriptors[] =
		{
			source->GetSRV(),
			m_filtered[TABLE_DOWN_SAMPLE].GetUAV()
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(uavSrvTable, utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);

		// Set Descriptor pools
		const DescriptorPool descriptorPools[] =
		{
			m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
			m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
		};
		commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

		commandList.SetComputePipelineLayout(m_pipelineLayouts[RESAMPLE]);
		commandList.SetPipelineState(m_pipelines[RESAMPLE]);
		commandList.SetComputeDescriptorTable(0, m_samplerTable);

		m_filtered[TABLE_DOWN_SAMPLE].Blit(commandList, 8, 8, 1, uavSrvTable, 1);
	}

	return true;
}

void Filter::Process(const CommandList& commandList, XMFLOAT2 focus, float sigma, ResourceState dstState)
{
	const uint8_t numPasses = m_numMips - 1;

	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Generate Mips
	ResourceBarrier barriers[2];
	auto numBarriers = 0u;
	if (numPasses > 0) numBarriers = m_filtered[TABLE_DOWN_SAMPLE].GenerateMips(commandList, barriers,
		8, 8, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE],
		m_pipelines[RESAMPLE], m_uavSrvTables[TABLE_DOWN_SAMPLE].data(), 1, m_samplerTable,
		0, numBarriers, nullptr, 0, 1, numPasses - 1);
	numBarriers = m_filtered[TABLE_UP_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers);
	commandList.Barrier(numBarriers, barriers);
	numBarriers = 0;

	commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses]);
	commandList.Dispatch(1, 1, 1);

	// Up sampling
	commandList.SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	GaussianConstants cb = { focus, sigma };
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		cb.Level = c - 1;
		commandList.SetCompute32BitConstants(2, SizeOfInUint32(GaussianConstants), &cb);
		numBarriers = m_filtered[TABLE_UP_SAMPLE].Blit(commandList, barriers, 8, 8, 1,
			cb.Level, c, dstState, m_uavSrvTables[TABLE_UP_SAMPLE][i], 1, numBarriers);
	}
}

void Filter::ProcessG(const CommandList& commandList, XMFLOAT2 focus, float sigma)
{
	const uint8_t numPasses = m_numMips > 0 ? m_numMips - 1 : 0;

	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Generate Mips
	commandList.SetComputePipelineLayout(m_pipelineLayouts[RESAMPLE]);
	commandList.SetPipelineState(m_pipelines[RESAMPLE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	ResourceBarrier barriers[2];
	auto numBarriers = 0u;
	if (numPasses > 0) numBarriers = m_filtered[TABLE_DOWN_SAMPLE].GenerateMips(commandList, barriers,
		8, 8, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE],
		m_pipelines[RESAMPLE], m_uavSrvTables[TABLE_DOWN_SAMPLE].data(), 1, m_samplerTable,
		0, numBarriers);
	numBarriers = m_filtered[TABLE_UP_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers, 0);
	commandList.Barrier(numBarriers, barriers);

	// Gaussian
	GaussianConstants cb = { focus, sigma, m_numMips };
	commandList.SetComputePipelineLayout(m_pipelineLayouts[GAUSSIAN]);
	commandList.SetCompute32BitConstants(2, SizeOfInUint32(GaussianConstants), &cb);
	m_filtered[TABLE_UP_SAMPLE].Blit(commandList, 8, 8, 1, m_uavSrvTables[TABLE_UP_SAMPLE][numPasses],
		1, 0, nullptr, 0, nullptr, 0, m_pipelines[GAUSSIAN]);
}

Texture2D& Filter::GetResult()
{
	return m_filtered[TABLE_UP_SAMPLE];
}

bool Filter::createPipelineLayouts()
{
	// Resampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[RESAMPLE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ResamplingLayout"), false);
	}

	// Up sampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(GaussianConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"UpSamplingLayout"), false);
	}

	// Gaussian
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(GaussianConstants), 0);
		X_RETURN(m_pipelineLayouts[GAUSSIAN], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"GaussianLayout"), false);
	}

	return true;
}

bool Filter::createPipelines()
{
	// Resampling
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, RESAMPLE, L"CSResample.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, RESAMPLE));
		X_RETURN(m_pipelines[RESAMPLE], state.GetPipeline(m_computePipelineCache, L"Resampling"), false);
	}

	// Up sampling
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, UP_SAMPLE, L"CSUpSample.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, UP_SAMPLE));
		X_RETURN(m_pipelines[UP_SAMPLE], state.GetPipeline(m_computePipelineCache, L"UpSampling"), false);
	}

	// Gaussian
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, GAUSSIAN, L"CSMipGaussian.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[GAUSSIAN]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, GAUSSIAN));
		X_RETURN(m_pipelines[GAUSSIAN], state.GetPipeline(m_computePipelineCache, L"GAUSSIAN"), false);
	}

	return true;
}

bool Filter::createDescriptorTables()
{
	const uint8_t numPasses = m_numMips > 0 ? m_numMips - 1 : 0;
	m_uavSrvTables[TABLE_DOWN_SAMPLE].resize(m_numMips);
	m_uavSrvTables[TABLE_UP_SAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		// Get UAV and SRVs
		{
			const Descriptor descriptors[] =
			{
				m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(i),
				m_filtered[TABLE_DOWN_SAMPLE].GetUAV(i + 1)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_DOWN_SAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
		}

		{
			const auto coarser = numPasses - i;
			const auto current = coarser - 1;
			const Descriptor descriptors[] =
			{
				m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(current),
				m_filtered[TABLE_UP_SAMPLE].GetSRVLevel(coarser),
				m_filtered[TABLE_UP_SAMPLE].GetUAV(current)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_UP_SAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
		}
	}

	// Get UAV and SRVs for the final-time down sampling
	{
		const Descriptor descriptors[] =
		{
			m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(numPasses - 1),
			m_filtered[TABLE_UP_SAMPLE].GetUAV(numPasses)
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Get UAV and SRVs for direct Gaussian
	{
		const Descriptor descriptors[] =
		{
			m_filtered[TABLE_DOWN_SAMPLE].GetSRV(),
			m_filtered[TABLE_UP_SAMPLE].GetUAV()
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_UP_SAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Create the sampler table
	Util::DescriptorTable samplerTable;
	const auto sampler = LINEAR_CLAMP;
	samplerTable.SetSamplers(0, 1, &sampler, m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(m_descriptorTableCache), false);

	return true;
}
