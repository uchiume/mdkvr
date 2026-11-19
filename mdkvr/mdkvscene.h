#ifndef MDKVSCENE_H
#define MDKVSCENE_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cmath>
#include <cstring>
#include <vector>
#include <map>

struct Vec2
{
	float x, y;

	Vec2() : x(0.0f), y(0.0f) {}
	Vec2(float fx, float fy) : x(fx), y(fy) {}
};

struct Vec3
{
	float x, y, z;

	Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
	Vec3(float fx, float fy, float fz) : x(fx), y(fy), z(fz) {}

	void normalize()
	{
		float len = sqrtf(x * x + y * y + z * z);
		if (len > 0.0f)
		{
			x /= len;
			y /= len;
			z /= len;
		}
	}
};

struct PackedVertex
{
	Vec3 position;
	Vec2 uv;
	Vec3 normal;

	bool operator<(const PackedVertex& that) const
	{
		return memcmp((void*)this, (void*)&that, sizeof(PackedVertex)) > 0;
	}
};

// CMdkvView が作成した Vulkan のオブジェクト
struct MdkvVulkanContext
{
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	VkQueue queue;
	VkCommandPool commandPool;
	VkRenderPass renderPass;
	VkSampleCountFlagBits samples;
};

class CMdkvScene
{
public:
	// 左右の目で別々の行列を使うので、ユニフォームバッファは目の数だけ領域を持つ
	static const uint32_t MAX_VIEWS = 2;

	CMdkvScene();
	virtual ~CMdkvScene();

	bool Init(const MdkvVulkanContext& context);
	void Draw(VkCommandBuffer commandBuffer, VkExtent2D extent, uint32_t nView, const glm::mat4& matProjection, const glm::mat4& matView);
	void Term();

private:
	MdkvVulkanContext m_context;
	VkDescriptorSetLayout m_descriptorSetLayout;
	VkDescriptorPool m_descriptorPool;
	VkDescriptorSet m_descriptorSet;
	VkPipelineLayout m_pipelineLayout;
	VkPipeline m_pipeline;
	VkBuffer m_vertexBuffer;
	VkDeviceMemory m_vertexMemory;
	VkBuffer m_indexBuffer;
	VkDeviceMemory m_indexMemory;
	VkBuffer m_uniformBuffer;
	VkDeviceMemory m_uniformMemory;
	void* m_pUniformMapped;
	VkDeviceSize m_nUniformStride;
	VkImage m_textureImage;
	VkDeviceMemory m_textureMemory;
	VkImageView m_textureView;
	VkSampler m_sampler;
	uint32_t m_nIndexCount;
	glm::mat4 m_Model;
	glm::vec3 m_LightPos;

	bool LoadTexture(int nResID);
	bool LoadModel(int nResID);
	bool LoadShader(HINSTANCE hInst, int nResID, char** ppBuf);
	bool LoadBinary(HINSTANCE hInst, int nResID, char** ppBuf, unsigned int* pnSize);
	VkShaderModule CreateShaderModule(int nResID);
	bool CreatePipeline(int nVertResID, int nFragResID);
	bool CreateDescriptorSet();
	bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, const void* pData, VkBuffer* pBuffer, VkDeviceMemory* pMemory);
	uint32_t FindMemoryType(uint32_t nTypeBits, VkMemoryPropertyFlags properties);
	VkCommandBuffer BeginOneTimeCommands();
	void EndOneTimeCommands(VkCommandBuffer commandBuffer);
	unsigned int LoadTGAFromBuffer(char* buff, unsigned int nSize, unsigned char** ppbuf, unsigned short* psWidth, unsigned short* psHeight);
	int LoadObjFromBuffer(float fScale, char* buff, std::vector<unsigned int>& vertexIndices, std::vector<unsigned int>& uvIndices, std::vector<unsigned int>& normalIndices, std::vector<Vec3>& temp_vertices, std::vector<Vec2>& temp_uvs, std::vector<Vec3>& temp_normals, Vec3* pMax, Vec3* pMin);
	bool ExtractObjModel(std::vector<unsigned int>& vertexIndices, std::vector<unsigned int>& uvIndices, std::vector<unsigned int>& normalIndices, std::vector<Vec3>& temp_vertices, std::vector<Vec2>& temp_uvs, std::vector<Vec3>& temp_normals, std::vector<Vec3>& out_vertices, std::vector<Vec2>& out_uvs, std::vector<Vec3>& out_normals);
	bool ReIndexObjModel(std::vector<Vec3>& in_vertices, std::vector<Vec2>& in_uvs, std::vector<Vec3>& in_normals, std::vector<unsigned short>& out_indices, std::vector<float>& out_vertices);
	bool getSimilarVertexIndex_fast(PackedVertex& packed, std::map<PackedVertex, unsigned int>& VertexToOutIndex, unsigned int& result);
};

#endif
