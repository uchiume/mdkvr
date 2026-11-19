#include "pch.h"
#include "mdkvscene.h"
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <string>
#include <sstream>
#include <cfloat>

#include "resource.h"

extern HINSTANCE g_hInst;

#pragma pack(push, 1)
struct TargaHeader
{
	unsigned char idLength;
	unsigned char colorMapType;
	unsigned char imageType;
	unsigned short colorMapOrigin;
	unsigned short colorMapLength;
	unsigned char colorMapDepth;
	unsigned short xOrigin;
	unsigned short yOrigin;
	unsigned short width;
	unsigned short height;
	unsigned char bpp;
	unsigned char descriptor;
};
#pragma pack(pop)

// シェーダーの SceneUniforms ブロック (std140) と同じ並び
struct SceneUniforms
{
	glm::mat4 MVP;
	glm::mat4 V;
	glm::mat4 M;
	glm::vec4 LightPosition_worldspace;
};

CMdkvScene::CMdkvScene()
	: m_context()
	, m_descriptorSetLayout(VK_NULL_HANDLE)
	, m_descriptorPool(VK_NULL_HANDLE)
	, m_descriptorSet(VK_NULL_HANDLE)
	, m_pipelineLayout(VK_NULL_HANDLE)
	, m_pipeline(VK_NULL_HANDLE)
	, m_vertexBuffer(VK_NULL_HANDLE)
	, m_vertexMemory(VK_NULL_HANDLE)
	, m_indexBuffer(VK_NULL_HANDLE)
	, m_indexMemory(VK_NULL_HANDLE)
	, m_uniformBuffer(VK_NULL_HANDLE)
	, m_uniformMemory(VK_NULL_HANDLE)
	, m_pUniformMapped(NULL)
	, m_nUniformStride(0)
	, m_textureImage(VK_NULL_HANDLE)
	, m_textureMemory(VK_NULL_HANDLE)
	, m_textureView(VK_NULL_HANDLE)
	, m_sampler(VK_NULL_HANDLE)
	, m_nIndexCount(0)
	, m_Model(1.0f)
	, m_LightPos(0.0f)
{
}

CMdkvScene::~CMdkvScene()
{
}

bool CMdkvScene::Init(const MdkvVulkanContext& context)
{
	m_context = context;

	// Model matrix : トラッキング空間 (床が y=0, 単位 m) で、正面 1.5m・目線の高さに 0.6m 角のキューブを置く
	// (cube.txt は ±1 の大きさ。一様スケールなのでシェーダの法線計算はそのままで良い)
	m_Model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.2f, -1.5f))
		* glm::scale(glm::mat4(1.0f), glm::vec3(0.3f));

	// ライト位置 (ワールド座標)
	m_LightPos = glm::vec3(4.0f, 5.0f, 3.0f);

	// 目ごとの行列を書く領域を、動的オフセットの境界に合わせて並べる
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(m_context.physicalDevice, &properties);
	VkDeviceSize nAlignment = properties.limits.minUniformBufferOffsetAlignment;
	m_nUniformStride = sizeof(SceneUniforms);
	if (nAlignment > 0)
	{
		m_nUniformStride = (m_nUniformStride + nAlignment - 1) & ~(nAlignment - 1);
	}

	// 毎フレーム CPU から書き込むので、ユニフォームバッファはマップしたままにする
	if (!CreateBuffer(m_nUniformStride * MAX_VIEWS, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL, &m_uniformBuffer, &m_uniformMemory))
	{
		return false;
	}
	if (vkMapMemory(m_context.device, m_uniformMemory, 0, VK_WHOLE_SIZE, 0, &m_pUniformMapped) != VK_SUCCESS)
	{
		m_pUniformMapped = NULL;
		return false;
	}

	if (!LoadTexture(IDR_CUBE_TGA))
	{
		return false;
	}

	if (!LoadModel(IDR_CUBE_OBJ))
	{
		return false;
	}

	if (!CreateDescriptorSet())
	{
		return false;
	}

	if (!CreatePipeline(IDR_SCENE_VSHADER, IDR_SCENE_FSHADER))
	{
		return false;
	}

	return true;
}

bool CMdkvScene::LoadTexture(int nResID)
{
	char* pData = NULL;
	unsigned int nSize = 0;
	if (!LoadBinary(g_hInst, nResID, &pData, &nSize))
	{
		return false;
	}

	unsigned char* pImage = NULL;
	unsigned short sWidth = 0;
	unsigned short sHeight = 0;
	unsigned int nImageSize = LoadTGAFromBuffer(pData, nSize, &pImage, &sWidth, &sHeight);
	delete[] pData;
	if (nImageSize == 0)
	{
		return false;
	}

	// 画素データをステージングバッファに置いて、GPU 側のイメージにコピーする
	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	bool bStaging = CreateBuffer(nImageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, pImage, &stagingBuffer, &stagingMemory);
	delete[] pImage;
	if (!bStaging)
	{
		return false;
	}

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.extent = { sWidth, sHeight, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bool bResult = vkCreateImage(m_context.device, &imageInfo, NULL, &m_textureImage) == VK_SUCCESS;

	if (bResult)
	{
		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements(m_context.device, m_textureImage, &memReq);
		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;
		allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		bResult = vkAllocateMemory(m_context.device, &allocInfo, NULL, &m_textureMemory) == VK_SUCCESS
			&& vkBindImageMemory(m_context.device, m_textureImage, m_textureMemory, 0) == VK_SUCCESS;
	}

	if (bResult)
	{
		VkCommandBuffer commandBuffer = BeginOneTimeCommands();

		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = m_textureImage;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

		VkBufferImageCopy region = {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { sWidth, sHeight, 1 };
		vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, m_textureImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

		EndOneTimeCommands(commandBuffer);
	}

	vkDestroyBuffer(m_context.device, stagingBuffer, NULL);
	vkFreeMemory(m_context.device, stagingMemory, NULL);
	if (!bResult)
	{
		return false;
	}

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = m_textureImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	if (vkCreateImageView(m_context.device, &viewInfo, NULL, &m_textureView) != VK_SUCCESS)
	{
		return false;
	}

	// LoadObjFromBuffer は V 座標を反転するので、繰り返しで 0..1 に折り返す
	VkSamplerCreateInfo samplerInfo = {};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.maxLod = 0.0f;
	if (vkCreateSampler(m_context.device, &samplerInfo, NULL, &m_sampler) != VK_SUCCESS)
	{
		return false;
	}

	return true;
}

bool CMdkvScene::LoadModel(int nResID)
{
	char* pszObj = NULL;
	if (!LoadShader(g_hInst, nResID, &pszObj))
	{
		return false;
	}

	std::vector<unsigned int> vertexIndices, uvIndices, normalIndices;
	std::vector<Vec3> temp_vertices;
	std::vector<Vec2> temp_uvs;
	std::vector<Vec3> temp_normals;
	Vec3 vMax, vMin;
	int nMode = LoadObjFromBuffer(1.0f, pszObj, vertexIndices, uvIndices, normalIndices, temp_vertices, temp_uvs, temp_normals, &vMax, &vMin);
	delete[] pszObj;
	if (nMode == 0)
	{
		return false;
	}

	std::vector<Vec3> vertices;
	std::vector<Vec2> uvs;
	std::vector<Vec3> normals;
	if (!ExtractObjModel(vertexIndices, uvIndices, normalIndices, temp_vertices, temp_uvs, temp_normals, vertices, uvs, normals))
	{
		return false;
	}
	// 同じ頂点をまとめて、頂点 (位置, 法線, UV) とインデックスに分ける
	std::vector<unsigned short> indices;
	std::vector<float> indexed_vertices;
	if (!ReIndexObjModel(vertices, uvs, normals, indices, indexed_vertices))
	{
		return false;
	}
	m_nIndexCount = (uint32_t)indices.size();

	VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if (!CreateBuffer(indexed_vertices.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hostVisible, &indexed_vertices[0], &m_vertexBuffer, &m_vertexMemory))
	{
		return false;
	}

	if (!CreateBuffer(indices.size() * sizeof(unsigned short), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hostVisible, &indices[0], &m_indexBuffer, &m_indexMemory))
	{
		return false;
	}

	return true;
}

bool CMdkvScene::LoadShader(HINSTANCE hInst, int nResID, char** ppBuf)
{
	HRSRC hRes = ::FindResourceA(hInst, MAKEINTRESOURCEA(nResID), "TEXTFILE");
	if (hRes == NULL)
	{
		return false;
	}
	DWORD dwSize = ::SizeofResource(hInst, hRes);
	if (dwSize == 0)
	{
		return false;
	}
	HGLOBAL hMem = ::LoadResource(hInst, hRes);
	if (hMem == NULL)
	{
		return false;
	}
	LPVOID pMem = ::LockResource(hMem);
	if (pMem == NULL)
	{
		::FreeResource(hMem);
		return false;
	}

	char* pBuf = new char[dwSize + 1];

	if (pBuf == NULL)
	{
		::FreeResource(hMem);
		return false;
	}

	strncpy_s(pBuf, dwSize + 1, (char*)pMem, (size_t)dwSize);

	pBuf[dwSize] = '\0';
	*ppBuf = pBuf;

	::FreeResource(hMem);

	return true;
}

bool CMdkvScene::LoadBinary(HINSTANCE hInst, int nResID, char** ppBuf, unsigned int* pnSize)
{
	HRSRC hRes = ::FindResource(hInst, MAKEINTRESOURCE(nResID), RT_RCDATA);
	if (hRes == NULL)
	{
		return false;
	}
	DWORD dwSize = ::SizeofResource(hInst, hRes);
	if (dwSize == 0)
	{
		return false;
	}
	HGLOBAL hMem = ::LoadResource(hInst, hRes);
	if (hMem == NULL)
	{
		return false;
	}
	LPVOID pMem = ::LockResource(hMem);
	if (pMem == NULL)
	{
		return false;
	}

	char* pBuf = new char[dwSize];
	memcpy_s(pBuf, dwSize, pMem, dwSize);
	*ppBuf = pBuf;
	*pnSize = dwSize;

	return true;
}

VkShaderModule CMdkvScene::CreateShaderModule(int nResID)
{
	// シェーダーはビルド時に glslc で SPIR-V にコンパイルしてリソースに入れている
	char* pCode = NULL;
	unsigned int nSize = 0;
	if (!LoadBinary(g_hInst, nResID, &pCode, &nSize))
	{
		return VK_NULL_HANDLE;
	}

	VkShaderModuleCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = nSize;
	createInfo.pCode = (const uint32_t*)pCode;
	VkShaderModule shaderModule = VK_NULL_HANDLE;
	if (vkCreateShaderModule(m_context.device, &createInfo, NULL, &shaderModule) != VK_SUCCESS)
	{
		shaderModule = VK_NULL_HANDLE;
	}
	delete[] pCode;

	return shaderModule;
}

bool CMdkvScene::CreatePipeline(int nVertResID, int nFragResID)
{
	VkShaderModule vertShader = CreateShaderModule(nVertResID);
	VkShaderModule fragShader = CreateShaderModule(nFragResID);
	if (vertShader == VK_NULL_HANDLE || fragShader == VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(m_context.device, vertShader, NULL);
		vkDestroyShaderModule(m_context.device, fragShader, NULL);
		return false;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertShader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragShader;
	stages[1].pName = "main";

	// 頂点バッファは 位置(3), 法線(3), UV(2) の順に並んでいる
	VkVertexInputBindingDescription binding = { 0, 8 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attributes[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },                 // vertexPosition_modelspace
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 6 * sizeof(float) },    // vertexUV
		{ 2, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float) }, // vertexNormal_modelspace
	};
	VkPipelineVertexInputStateCreateInfo vertexInput = {};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = 3;
	vertexInput.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	// ビューポートとシザーは描画時に設定する
	VkPipelineViewportStateCreateInfo viewportState = {};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;
	VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkPipelineRasterizationStateCreateInfo rasterization = {};
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;

	// 目ごとの描画先 (CMdkvView の MSAA バッファ) とサンプル数を合わせる
	VkPipelineMultisampleStateCreateInfo multisample = {};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = m_context.samples;

	// Enable depth test : Accept fragment if it is closer to the camera than the former one
	VkPipelineDepthStencilStateCreateInfo depthStencil = {};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo colorBlend = {};
	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments = &blendAttachment;

	VkPipelineLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &m_descriptorSetLayout;
	bool bResult = vkCreatePipelineLayout(m_context.device, &layoutInfo, NULL, &m_pipelineLayout) == VK_SUCCESS;

	if (bResult)
	{
		VkGraphicsPipelineCreateInfo pipelineInfo = {};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = stages;
		pipelineInfo.pVertexInputState = &vertexInput;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterization;
		pipelineInfo.pMultisampleState = &multisample;
		pipelineInfo.pDepthStencilState = &depthStencil;
		pipelineInfo.pColorBlendState = &colorBlend;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.layout = m_pipelineLayout;
		pipelineInfo.renderPass = m_context.renderPass;
		pipelineInfo.subpass = 0;
		bResult = vkCreateGraphicsPipelines(m_context.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &m_pipeline) == VK_SUCCESS;
	}

	vkDestroyShaderModule(m_context.device, vertShader, NULL);
	vkDestroyShaderModule(m_context.device, fragShader, NULL);

	return bResult;
}

bool CMdkvScene::CreateDescriptorSet()
{
	// binding 0 : SceneUniforms (目ごとの領域を動的オフセットで選ぶ), binding 1 : myTextureSampler
	VkDescriptorSetLayoutBinding bindings[2] = {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 2;
	layoutInfo.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(m_context.device, &layoutInfo, NULL, &m_descriptorSetLayout) != VK_SUCCESS)
	{
		return false;
	}

	VkDescriptorPoolSize poolSizes[2] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
	};
	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = poolSizes;
	if (vkCreateDescriptorPool(m_context.device, &poolInfo, NULL, &m_descriptorPool) != VK_SUCCESS)
	{
		return false;
	}

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_descriptorSetLayout;
	if (vkAllocateDescriptorSets(m_context.device, &allocInfo, &m_descriptorSet) != VK_SUCCESS)
	{
		return false;
	}

	VkDescriptorBufferInfo bufferInfo = { m_uniformBuffer, 0, sizeof(SceneUniforms) };
	VkDescriptorImageInfo imageInfo = { m_sampler, m_textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet writes[2] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = m_descriptorSet;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	writes[0].pBufferInfo = &bufferInfo;
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = m_descriptorSet;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = &imageInfo;
	vkUpdateDescriptorSets(m_context.device, 2, writes, 0, NULL);

	return true;
}

bool CMdkvScene::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, const void* pData, VkBuffer* pBuffer, VkDeviceMemory* pMemory)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(m_context.device, &bufferInfo, NULL, pBuffer) != VK_SUCCESS)
	{
		return false;
	}

	VkMemoryRequirements memReq;
	vkGetBufferMemoryRequirements(m_context.device, *pBuffer, &memReq);
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, properties);
	if (vkAllocateMemory(m_context.device, &allocInfo, NULL, pMemory) != VK_SUCCESS)
	{
		return false;
	}
	vkBindBufferMemory(m_context.device, *pBuffer, *pMemory, 0);

	// pData があればバッファにコピーする (HOST_VISIBLE のメモリのみ)
	if (pData != NULL)
	{
		void* pMapped = NULL;
		if (vkMapMemory(m_context.device, *pMemory, 0, size, 0, &pMapped) != VK_SUCCESS)
		{
			return false;
		}
		memcpy(pMapped, pData, (size_t)size);
		vkUnmapMemory(m_context.device, *pMemory);
	}

	return true;
}

uint32_t CMdkvScene::FindMemoryType(uint32_t nTypeBits, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(m_context.physicalDevice, &memProperties);
	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if ((nTypeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}
	return 0;
}

VkCommandBuffer CMdkvScene::BeginOneTimeCommands()
{
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_context.commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	vkAllocateCommandBuffers(m_context.device, &allocInfo, &commandBuffer);

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	return commandBuffer;
}

void CMdkvScene::EndOneTimeCommands(VkCommandBuffer commandBuffer)
{
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	vkQueueSubmit(m_context.queue, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(m_context.queue);

	vkFreeCommandBuffers(m_context.device, m_context.commandPool, 1, &commandBuffer);
}

unsigned int CMdkvScene::LoadTGAFromBuffer(char* buff, unsigned int nSize, unsigned char** ppbuf, unsigned short* psWidth, unsigned short* psHeight)
{
	int width, height, bpp, imageSize;
	unsigned char* targaImage;

	if (nSize < sizeof(TargaHeader))
	{
		return 0;
	}
	TargaHeader header;
	memcpy_s(&header, 18, buff, 18);
	width = (int)header.width;
	height = (int)header.height;
	imageSize = width * height * 4;
	bpp = header.bpp;
	if (bpp != 32)
	{
		return 0;
	}

	if (header.imageType != 2 && header.imageType != 10)
	{
		return 0;
	}
	char* pPos = buff + 18 + header.idLength;
	char* pEnd = buff + nSize;
	if (header.imageType == 2 && pEnd - pPos < imageSize)
	{
		return 0;
	}
	targaImage = new unsigned char[imageSize];
	if (!targaImage)
	{
		return 0;
	}
	int bytePerPixel = bpp / 8;
	if (header.imageType == 2)
	{
		errno_t err = memcpy_s(targaImage, imageSize, pPos, imageSize);
		if (err != 0) {
			delete[] targaImage;
			return 0;
		}
	}
	else
	{
		// RLE compressed
		int nPixel = 0;
		int nPixelCount = width * height;
		while (nPixel < nPixelCount)
		{
			if (pPos >= pEnd)
			{
				delete[] targaImage;
				return 0;
			}
			unsigned char packet = (unsigned char)*pPos++;
			int count = (packet & 0x7F) + 1;
			bool bRepeat = (packet & 0x80) != 0;
			int nDataSize = bRepeat ? bytePerPixel : count * bytePerPixel;
			if (nPixel + count > nPixelCount || pEnd - pPos < nDataSize)
			{
				delete[] targaImage;
				return 0;
			}
			for (int i = 0; i < count; i++)
			{
				memcpy(targaImage + (nPixel + i) * bytePerPixel, bRepeat ? pPos : pPos + i * bytePerPixel, bytePerPixel);
			}
			pPos += nDataSize;
			nPixel += count;
		}
	}
	unsigned int temp;
	// swap BGR(A) to RGB(A)
	for (int i = 0; i < imageSize; i += bytePerPixel) {
		temp = targaImage[i];
		targaImage[i] = targaImage[i + 2];
		targaImage[i + 2] = temp;
	}
	*ppbuf = targaImage;
	*psWidth = width;
	*psHeight = height;

	return imageSize;
}

int CMdkvScene::LoadObjFromBuffer(float fScale, char* buff, std::vector<unsigned int>& vertexIndices, std::vector<unsigned int>& uvIndices, std::vector<unsigned int>& normalIndices, std::vector<Vec3>& temp_vertices, std::vector<Vec2>& temp_uvs, std::vector<Vec3>& temp_normals, Vec3* pMax, Vec3* pMin)
{
	int nMode = 0;	// auto detect
	float fXmax, fXmin, fYmax, fYmin, fZmax, fZmin;
	fXmax = -FLT_MAX; fXmin = FLT_MAX; fYmax = -FLT_MAX; fYmin = FLT_MAX; fZmax = -FLT_MAX; fZmin = FLT_MAX;

	std::string str(buff);
	std::istringstream stream(str);
	std::string field;
	char delimiter = '\n';

	while (std::getline(stream, field, delimiter))
	{
		if (field.substr(0, 2) == "v ")
		{
			Vec3 vertex;
			int nPos = (int)field.find(' ', 0);
			std::string val = field.substr(nPos + 1);
			int res = sscanf_s(val.c_str(), "%f %f %f", &vertex.x, &vertex.y, &vertex.z);
			if (res != 3) return 0;

			vertex.x = vertex.x * fScale;
			vertex.y = vertex.y * fScale;
			vertex.z = vertex.z * fScale;

			if (vertex.x > fXmax) fXmax = vertex.x;
			if (vertex.x < fXmin) fXmin = vertex.x;
			if (vertex.y > fYmax) fYmax = vertex.y;
			if (vertex.y < fYmin) fYmin = vertex.y;
			if (vertex.z > fZmax) fZmax = vertex.z;
			if (vertex.z < fZmin) fZmin = vertex.z;
			temp_vertices.push_back(vertex);
		}
		else if (field.substr(0, 3) == "vt ")
		{
			Vec2 uv;
			int nPos = (int)field.find(' ', 0);
			std::string val = field.substr(nPos + 1);
			int res = sscanf_s(val.c_str(), "%f %f", &uv.x, &uv.y);
			if (res != 2) return 0;

			uv.y = -uv.y;
			temp_uvs.push_back(uv);
		}
		else if (field.substr(0, 3) == "vn ")
		{
			Vec3 normal;
			int nPos = (int)field.find(' ', 0);
			std::string val = field.substr(nPos + 1);
			int res = sscanf_s(val.c_str(), "%f %f %f", &normal.x, &normal.y, &normal.z);
			if (res != 3) return 0;

			temp_normals.push_back(normal);
		}
		else if (field.substr(0, 2) == "f ")
		{
			unsigned int vertexIndex[4], uvIndex[4], normalIndex[4];
			int nPos = (int)field.find(' ', 0);
			std::string val = field.substr(nPos + 1);
			int nCount = 0;

			if (nMode == 0 || nMode == 1)
			{
				int res = sscanf_s(val.c_str(), "%u/%u/%u %u/%u/%u %u/%u/%u %u/%u/%u", &vertexIndex[0], &uvIndex[0], &normalIndex[0], &vertexIndex[1], &uvIndex[1], &normalIndex[1], &vertexIndex[2], &uvIndex[2], &normalIndex[2], &vertexIndex[3], &uvIndex[3], &normalIndex[3]);
				if (res == 9 || res == 12)
				{
					nMode = 1;
					nCount = res / 3;
				}
			}
			if (nCount == 0 && (nMode == 0 || nMode == 2))
			{
				int res = sscanf_s(val.c_str(), "%u//%u %u//%u %u//%u %u//%u", &vertexIndex[0], &normalIndex[0], &vertexIndex[1], &normalIndex[1], &vertexIndex[2], &normalIndex[2], &vertexIndex[3], &normalIndex[3]);
				if (res == 6 || res == 8)
				{
					nMode = 2;
					nCount = res / 2;
				}
			}
			if (nCount == 0 && (nMode == 0 || nMode == 3))
			{
				int res = sscanf_s(val.c_str(), "%u/%u %u/%u %u/%u %u/%u", &vertexIndex[0], &uvIndex[0], &vertexIndex[1], &uvIndex[1], &vertexIndex[2], &uvIndex[2], &vertexIndex[3], &uvIndex[3]);
				if (res == 6 || res == 8)
				{
					nMode = 3;
					nCount = res / 2;
				}
			}
			if (nCount == 0)
			{
				return 0;
			}

			static const int triangles[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
			for (int t = 0; t < nCount - 2; t++)
			{
				for (int k = 0; k < 3; k++)
				{
					int i = triangles[t][k];
					vertexIndices.push_back(vertexIndex[i] - 1);
					if (nMode != 2)
					{
						uvIndices.push_back(uvIndex[i] - 1);
					}
					if (nMode != 3)
					{
						normalIndices.push_back(normalIndex[i] - 1);
					}
				}
			}
		}
	}

	pMax->x = fXmax;
	pMax->y = fYmax;
	pMax->z = fZmax;
	pMin->x = fXmin;
	pMin->y = fYmin;
	pMin->z = fZmin;

	return nMode;
}

bool CMdkvScene::ExtractObjModel(std::vector<unsigned int>& vertexIndices, std::vector<unsigned int>& uvIndices, std::vector<unsigned int>& normalIndices, std::vector<Vec3>& temp_vertices, std::vector<Vec2>& temp_uvs, std::vector<Vec3>& temp_normals, std::vector<Vec3>& out_vertices, std::vector<Vec2>& out_uvs, std::vector<Vec3>& out_normals)
{
	if (vertexIndices.size() == 0)
		return false;

	if (uvIndices.size() == 0)
	{
		for (unsigned int i = 0; i < vertexIndices.size(); i++) {
			unsigned int vertexIndex = vertexIndices[i];
			unsigned int normalIndex = normalIndices[i];

			Vec3 vertex = temp_vertices[vertexIndex];
			Vec2 uv = Vec2(0, 0);
			Vec3 normal = temp_normals[normalIndex];
			normal.normalize();

			out_vertices.push_back(vertex);
			out_uvs.push_back(uv);
			out_normals.push_back(normal);
		}
	}
	else if (normalIndices.size() == 0)
	{
		for (unsigned int i = 0; i < vertexIndices.size(); i++) {
			unsigned int vertexIndex = vertexIndices[i];
			unsigned int uvIndex = uvIndices[i];

			Vec3 vertex = temp_vertices[vertexIndex];
			Vec2 uv = temp_uvs[uvIndex];
			Vec3 normal = Vec3(0, 0, 0);

			out_vertices.push_back(vertex);
			out_uvs.push_back(uv);
			out_normals.push_back(normal);
		}
	}
	else
	{
		for (unsigned int i = 0; i < vertexIndices.size(); i++) {
			unsigned int vertexIndex = vertexIndices[i];
			unsigned int uvIndex = uvIndices[i];
			unsigned int normalIndex = normalIndices[i];

			Vec3 vertex = temp_vertices[vertexIndex];
			Vec2 uv = temp_uvs[uvIndex];
			Vec3 normal = temp_normals[normalIndex];

			out_vertices.push_back(vertex);
			out_uvs.push_back(uv);
			out_normals.push_back(normal);
		}
	}

	return true;
}

bool CMdkvScene::ReIndexObjModel(std::vector<Vec3>& in_vertices,
	std::vector<Vec2>& in_uvs,
	std::vector<Vec3>& in_normals,
	std::vector<unsigned short>& out_indices,
	std::vector<float>& out_vertices)
{
	std::map<PackedVertex, unsigned int> VertexToOutIndex;

	// For each input vertex
	for (unsigned int i = 0; i < in_vertices.size(); i++) {

		PackedVertex packed = { in_vertices[i], in_uvs[i], in_normals[i] };

		// Try to find a similar vertex in out_XXXX
		unsigned int index;
		bool found = getSimilarVertexIndex_fast(packed, VertexToOutIndex, index);

		if (found) { // A similar vertex is already in the VBO, use it instead !
			out_indices.push_back((unsigned short)index);
		}
		else { // If not, it needs to be added in the output data.
			out_vertices.push_back(in_vertices[i].x);
			out_vertices.push_back(in_vertices[i].y);
			out_vertices.push_back(in_vertices[i].z);
			out_vertices.push_back(in_normals[i].x);
			out_vertices.push_back(in_normals[i].y);
			out_vertices.push_back(in_normals[i].z);
			out_vertices.push_back(in_uvs[i].x);
			out_vertices.push_back(in_uvs[i].y);

			unsigned int newindex = (unsigned int)out_vertices.size() / 8 - 1;
			out_indices.push_back((unsigned short)newindex);
			VertexToOutIndex[packed] = newindex;
		}
	}

	return true;
}

bool CMdkvScene::getSimilarVertexIndex_fast(PackedVertex& packed, std::map<PackedVertex, unsigned int>& VertexToOutIndex, unsigned int& result)
{
	std::map<PackedVertex, unsigned int>::iterator it = VertexToOutIndex.find(packed);
	if (it == VertexToOutIndex.end()) {
		return false;
	}
	else {
		result = it->second;
		return true;
	}
}

void CMdkvScene::Draw(VkCommandBuffer commandBuffer, VkExtent2D extent, uint32_t nView, const glm::mat4& matProjection, const glm::mat4& matView)
{
	if (nView >= MAX_VIEWS)
	{
		return;
	}

	// この目の行列をユニフォームバッファの nView 番目の領域に書く
	// (前のフレームの GPU 処理は CMdkvView がフェンスで待ってから呼ぶので、書き換えて良い)
	SceneUniforms uniforms;
	uniforms.MVP = matProjection * matView * m_Model;
	uniforms.V = matView;
	uniforms.M = m_Model;
	uniforms.LightPosition_worldspace = glm::vec4(m_LightPos, 1.0f);
	uint32_t nOffset = (uint32_t)(m_nUniformStride * nView);
	memcpy((char*)m_pUniformMapped + nOffset, &uniforms, sizeof(uniforms));

	// Use our shader
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

	VkViewport viewport = { 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
	vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
	VkRect2D scissor = { { 0, 0 }, extent };
	vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

	// SceneUniforms (この目の領域) and myTextureSampler
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 1, &nOffset);

	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffer, &offset);
	vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT16);

	vkCmdDrawIndexed(commandBuffer, m_nIndexCount, 1, 0, 0, 0);
}

void CMdkvScene::Term()
{
	VkDevice device = m_context.device;
	if (device == VK_NULL_HANDLE)
	{
		return;
	}
	if (m_pUniformMapped != NULL)
	{
		vkUnmapMemory(device, m_uniformMemory);
		m_pUniformMapped = NULL;
	}
	vkDestroyPipeline(device, m_pipeline, NULL);
	vkDestroyPipelineLayout(device, m_pipelineLayout, NULL);
	vkDestroyDescriptorPool(device, m_descriptorPool, NULL);
	vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, NULL);
	vkDestroySampler(device, m_sampler, NULL);
	vkDestroyImageView(device, m_textureView, NULL);
	vkDestroyImage(device, m_textureImage, NULL);
	vkFreeMemory(device, m_textureMemory, NULL);
	vkDestroyBuffer(device, m_vertexBuffer, NULL);
	vkFreeMemory(device, m_vertexMemory, NULL);
	vkDestroyBuffer(device, m_indexBuffer, NULL);
	vkFreeMemory(device, m_indexMemory, NULL);
	vkDestroyBuffer(device, m_uniformBuffer, NULL);
	vkFreeMemory(device, m_uniformMemory, NULL);

	m_pipeline = VK_NULL_HANDLE;
	m_pipelineLayout = VK_NULL_HANDLE;
	m_descriptorPool = VK_NULL_HANDLE;
	m_descriptorSet = VK_NULL_HANDLE;
	m_descriptorSetLayout = VK_NULL_HANDLE;
	m_sampler = VK_NULL_HANDLE;
	m_textureView = VK_NULL_HANDLE;
	m_textureImage = VK_NULL_HANDLE;
	m_textureMemory = VK_NULL_HANDLE;
	m_vertexBuffer = VK_NULL_HANDLE;
	m_vertexMemory = VK_NULL_HANDLE;
	m_indexBuffer = VK_NULL_HANDLE;
	m_indexMemory = VK_NULL_HANDLE;
	m_uniformBuffer = VK_NULL_HANDLE;
	m_uniformMemory = VK_NULL_HANDLE;
}
