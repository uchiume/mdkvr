#ifndef MDKVVIEW_H
#define MDKVVIEW_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

extern HINSTANCE g_hInst;

class CMdkvScene;

class CMdkvView
{
public:
	CMdkvView();
	virtual ~CMdkvView();
	bool Init(int nWidth, int nHeight);
	void Draw();
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
	enum EEye
	{
		Eye_Left = 0,
		Eye_Right = 1,
		Eye_Count = 2,
	};

	// 目ごとの描画先
	struct FramebufferDesc
	{
		VkImage m_depthImage;			// MSAA 深度
		VkDeviceMemory m_depthMemory;
		VkImageView m_depthImageView;
		VkImage m_renderImage;			// MSAA 色
		VkDeviceMemory m_renderMemory;
		VkImageView m_renderImageView;
		VkImage m_resolveImage;			// resolve 先 (swapchain へのコピー元、コンパニオンウィンドウ表示)
		VkDeviceMemory m_resolveMemory;
		VkImageView m_resolveImageView;
		VkFramebuffer m_framebuffer;
		XrSwapchain m_swapchain;
		std::vector<XrSwapchainImageVulkan2KHR> m_swapchainImages;
		uint32_t m_nAcquiredIndex;
		bool m_bAcquired;				// xrAcquireSwapchainImage 済みで、まだ release していない
	};

	// Vulkan
	bool InitVulkan(XrGraphicsRequirementsVulkan2KHR& requirements);
	void TermVulkan();
	bool CreateRenderPass();
	bool CreateImage(VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect,
		VkImage* pImage, VkDeviceMemory* pMemory, VkImageView* pView);
	uint32_t FindMemoryType(uint32_t nTypeBits, VkMemoryPropertyFlags properties);
	bool ShowError(const char* pszMessage);

	// XR
	bool InitVR();
	void TermVR();
	void ProcessVREvents();
	bool UpdateHMDMatrixPose(XrTime predictedDisplayTime);
	glm::mat4 GetHMDMatrixProjectionEye(EEye nEye);
	glm::mat4 GetHMDMatrixPoseEye(EEye nEye);
	glm::mat4 GetCurrentViewMatrix(EEye nEye);
	static glm::mat4 ConvertXrPose(const XrPosef& pose);
	bool ShowXrError(XrResult result, const char* pszFunction);

	// Render targets
	bool SetupStereoRenderTargets();
	bool CreateFrameBuffer(int nWidth, int nHeight, FramebufferDesc& desc);
	void DeleteFrameBuffer(FramebufferDesc& desc);
	bool RenderStereoTargets(std::vector<XrCompositionLayerProjectionView>& projectionViews);
	bool RenderEye(EEye nEye, FramebufferDesc& desc);
	void ReleaseEyeImages();

	// Companion window (Vulkan のスワップチェーン)
	bool SetupCompanionWindow();
	bool CreateSwapchain();
	void DestroySwapchain();
	void RecreateSwapchain();
	void RenderCompanionWindow(uint32_t nImageIndex);

	HWND m_hWnd;
	CMdkvScene* m_pScene;

	int m_nWidth;	// companion window size
	int m_nHeight;

	// Vulkan
	VkInstance m_vkInstance;
	VkSurfaceKHR m_surface;
	VkPhysicalDevice m_physicalDevice;
	VkDevice m_device;
	uint32_t m_nQueueFamily;
	VkQueue m_queue;
	VkSurfaceFormatKHR m_surfaceFormat;
	VkFormat m_depthFormat;
	VkFormat m_colorFormat;				// 目ごとの描画に使う形式 (swapchain と同じ並びの UNORM)
	VkSampleCountFlagBits m_samples;
	VkRenderPass m_renderPass;
	VkCommandPool m_commandPool;
	VkCommandBuffer m_commandBuffer;
	VkSemaphore m_imageAvailable;
	VkFence m_inFlight;
	bool m_bEyeRendered;				// resolve 画像に一度でも描いたか

	// ウィンドウサイズが変わったら作り直すもの
	VkSwapchainKHR m_swapchain;
	VkExtent2D m_extent;
	std::vector<VkImage> m_swapchainImages;
	std::vector<VkSemaphore> m_renderFinished;

	// OpenXR
	XrInstance m_instance;
	XrSystemId m_systemId;
	XrSession m_session;
	XrSpace m_appSpace;
	XrViewConfigurationType m_viewConfigType;
	XrEnvironmentBlendMode m_blendMode;
	XrSessionState m_sessionState;
	bool m_bSessionRunning;
	int64_t m_nColorSwapchainFormat;
	std::vector<XrViewConfigurationView> m_configViews;
	std::vector<XrView> m_views;

	glm::mat4 m_mat4EyePosLeft;		// view matrix (左目)
	glm::mat4 m_mat4EyePosRight;	// view matrix (右目)
	glm::mat4 m_mat4ProjectionLeft;
	glm::mat4 m_mat4ProjectionRight;
	float m_fNearClip;
	float m_fFarClip;

	uint32_t m_nRenderWidth;
	uint32_t m_nRenderHeight;
	FramebufferDesc m_leftEyeDesc;
	FramebufferDesc m_rightEyeDesc;
};

#endif
