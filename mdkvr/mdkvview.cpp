#include "pch.h"
#include "resource.h"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <string>
#include <cmath>
#include "mdkvscene.h"
#include "mdkvview.h"

CMdkvView::CMdkvView() :
	m_hWnd(nullptr),
	m_pScene(nullptr),
	m_nWidth(0),
	m_nHeight(0),
	m_vkInstance(VK_NULL_HANDLE),
	m_surface(VK_NULL_HANDLE),
	m_physicalDevice(VK_NULL_HANDLE),
	m_device(VK_NULL_HANDLE),
	m_nQueueFamily(0),
	m_queue(VK_NULL_HANDLE),
	m_surfaceFormat(),
	m_depthFormat(VK_FORMAT_D32_SFLOAT),
	m_colorFormat(VK_FORMAT_R8G8B8A8_UNORM),
	m_samples(VK_SAMPLE_COUNT_1_BIT),
	m_renderPass(VK_NULL_HANDLE),
	m_commandPool(VK_NULL_HANDLE),
	m_commandBuffer(VK_NULL_HANDLE),
	m_imageAvailable(VK_NULL_HANDLE),
	m_inFlight(VK_NULL_HANDLE),
	m_bEyeRendered(false),
	m_swapchain(VK_NULL_HANDLE),
	m_extent(),
	m_instance(XR_NULL_HANDLE),
	m_systemId(XR_NULL_SYSTEM_ID),
	m_session(XR_NULL_HANDLE),
	m_appSpace(XR_NULL_HANDLE),
	m_viewConfigType(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO),
	m_blendMode(XR_ENVIRONMENT_BLEND_MODE_OPAQUE),
	m_sessionState(XR_SESSION_STATE_UNKNOWN),
	m_bSessionRunning(false),
	m_nColorSwapchainFormat(0),
	m_mat4EyePosLeft(1.0f),
	m_mat4EyePosRight(1.0f),
	m_mat4ProjectionLeft(1.0f),
	m_mat4ProjectionRight(1.0f),
	m_fNearClip(0.1f),
	m_fFarClip(30.0f),
	m_nRenderWidth(0),
	m_nRenderHeight(0),
	m_leftEyeDesc(),
	m_rightEyeDesc()
{
}

CMdkvView::~CMdkvView()
{
}

LRESULT CALLBACK CMdkvView::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	CMdkvView* thisPtr = (CMdkvView*)::GetWindowLongPtr(hWnd, GWLP_USERDATA);
	if (!thisPtr) {
		if (msg == WM_CREATE) {
			thisPtr = (CMdkvView*)((LPCREATESTRUCT)lParam)->lpCreateParams;
			if (thisPtr) {
				::SetWindowLongPtr(hWnd, GWLP_USERDATA, (ULONG_PTR)thisPtr);
			}
		}
		if (!thisPtr) {
			return DefWindowProc(hWnd, msg, wParam, lParam);
		}
	}

	switch (msg) {
	case WM_CREATE:
		break;
	case WM_CLOSE:
		::DestroyWindow(hWnd);
		break;
	case WM_DESTROY:
		thisPtr->TermVulkan();
		::PostQuitMessage(0);
		break;
	default:
		break;
	}
	return ::DefWindowProc(hWnd, msg, wParam, lParam);
}

bool CMdkvView::Init(int nWidth, int nHeight)
{
	WNDCLASSEX wndClass;
	memset(&wndClass, 0, sizeof(wndClass));
	wndClass.cbSize = sizeof(wndClass);
	wndClass.style = CS_OWNDC;
	wndClass.lpszClassName = L"MDKV_VIEW_CLASS";
	wndClass.lpfnWndProc = this->WndProc;
	wndClass.hInstance = g_hInst;
	wndClass.hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_ICON1));
	wndClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wndClass.hCursor = ::LoadCursor(NULL, IDC_ARROW);
	::RegisterClassEx(&wndClass);

	m_hWnd = ::CreateWindowEx(
		WS_EX_APPWINDOW | WS_EX_WINDOWEDGE,
		L"MDKV_VIEW_CLASS",
		L"MDKVR",
		WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SIZEBOX,
		0,
		0,
		nWidth,
		nHeight,
		nullptr,
		nullptr,
		g_hInst,
		this);

	if (m_hWnd == NULL)
	{
		return false;
	}

	m_nWidth = nWidth;	// companion window size
	m_nHeight = nHeight;

	// OpenXR → Vulkan → OpenXR セッションの順に初期化する (InitVR の中で InitVulkan を呼ぶ)
	bool bResult = InitVR() && SetupStereoRenderTargets();
	if (bResult)
	{
		MdkvVulkanContext context = { m_physicalDevice, m_device, m_queue, m_commandPool, m_renderPass, m_samples };
		m_pScene = new CMdkvScene();
		bResult = m_pScene->Init(context);
	}
	if (bResult)
	{
		bResult = SetupCompanionWindow();
	}

	if (!bResult)
	{
		::DestroyWindow(m_hWnd);	// WM_DESTROY で TermVulkan が呼ばれる
		m_hWnd = NULL;
		return false;
	}

	::ShowWindow(m_hWnd, SW_SHOW);

	return true;
}

bool CMdkvView::ShowError(const char* pszMessage)
{
	::MessageBoxA(m_hWnd, pszMessage, "MDKVR Error", MB_OK | MB_ICONERROR);
	return false;
}

//-----------------------------------------------------------------------------
// Vulkan
//-----------------------------------------------------------------------------
bool CMdkvView::InitVulkan(XrGraphicsRequirementsVulkan2KHR& requirements)
{
	XrResult xrResult;
	VkResult vkResult = VK_SUCCESS;

	// OpenXR 経由で Vulkan のインスタンスとデバイスを作るための関数を取得する
	PFN_xrCreateVulkanInstanceKHR pfnCreateVulkanInstanceKHR = nullptr;
	PFN_xrGetVulkanGraphicsDevice2KHR pfnGetVulkanGraphicsDevice2KHR = nullptr;
	PFN_xrCreateVulkanDeviceKHR pfnCreateVulkanDeviceKHR = nullptr;
	if (XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrCreateVulkanInstanceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateVulkanInstanceKHR)))
		|| XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanGraphicsDevice2KHR)))
		|| XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrCreateVulkanDeviceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateVulkanDeviceKHR))))
	{
		return ShowError("Failed to get the OpenXR Vulkan functions.");
	}

	// Instance (コンパニオンウィンドウ用にサーフェスの拡張を有効にする)
	uint32_t nApiVersion = VK_API_VERSION_1_0;
	if (requirements.minApiVersionSupported > XR_MAKE_VERSION(1, 0, 0))
	{
		nApiVersion = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported), 0);
	}
	const char* instanceExtensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "MDKVR";
	appInfo.apiVersion = nApiVersion;
	VkInstanceCreateInfo instanceInfo = {};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &appInfo;
	instanceInfo.enabledExtensionCount = (uint32_t)_countof(instanceExtensions);
	instanceInfo.ppEnabledExtensionNames = instanceExtensions;

	XrVulkanInstanceCreateInfoKHR xrInstanceInfo{ XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
	xrInstanceInfo.systemId = m_systemId;
	xrInstanceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
	xrResult = pfnCreateVulkanInstanceKHR(m_instance, &xrInstanceInfo, &m_vkInstance, &vkResult);
	if (XR_FAILED(xrResult))
	{
		return ShowXrError(xrResult, "xrCreateVulkanInstanceKHR");
	}
	if (vkResult != VK_SUCCESS)
	{
		return ShowError("vkCreateInstance failed.");
	}

	// Surface
	VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
	surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	surfaceInfo.hinstance = g_hInst;
	surfaceInfo.hwnd = m_hWnd;
	if (vkCreateWin32SurfaceKHR(m_vkInstance, &surfaceInfo, NULL, &m_surface) != VK_SUCCESS)
	{
		return ShowError("vkCreateWin32SurfaceKHR failed.");
	}

	// GPU は HMD がつながっているものを OpenXR に決めてもらう
	XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
	deviceGetInfo.systemId = m_systemId;
	deviceGetInfo.vulkanInstance = m_vkInstance;
	xrResult = pfnGetVulkanGraphicsDevice2KHR(m_instance, &deviceGetInfo, &m_physicalDevice);
	if (XR_FAILED(xrResult))
	{
		return ShowXrError(xrResult, "xrGetVulkanGraphicsDevice2KHR");
	}

	// 描画と (コンパニオンウィンドウへの) 表示ができるキューを選ぶ
	uint32_t nFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &nFamilyCount, NULL);
	std::vector<VkQueueFamilyProperties> families(nFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &nFamilyCount, families.data());
	bool bFound = false;
	for (uint32_t i = 0; i < nFamilyCount; i++)
	{
		VkBool32 bPresent = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(m_physicalDevice, i, m_surface, &bPresent);
		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && bPresent)
		{
			m_nQueueFamily = i;
			bFound = true;
			break;
		}
	}
	if (!bFound)
	{
		return ShowError("No Vulkan queue supports both graphics and presentation.");
	}

	// Logical device
	float fPriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {};
	queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = m_nQueueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &fPriority;
	const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo deviceInfo = {};
	deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.enabledExtensionCount = (uint32_t)_countof(deviceExtensions);
	deviceInfo.ppEnabledExtensionNames = deviceExtensions;

	XrVulkanDeviceCreateInfoKHR xrDeviceInfo{ XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
	xrDeviceInfo.systemId = m_systemId;
	xrDeviceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
	xrDeviceInfo.vulkanPhysicalDevice = m_physicalDevice;
	xrResult = pfnCreateVulkanDeviceKHR(m_instance, &xrDeviceInfo, &m_device, &vkResult);
	if (XR_FAILED(xrResult))
	{
		return ShowXrError(xrResult, "xrCreateVulkanDeviceKHR");
	}
	if (vkResult != VK_SUCCESS)
	{
		m_device = VK_NULL_HANDLE;
		return ShowError("vkCreateDevice failed.");
	}
	vkGetDeviceQueue(m_device, m_nQueueFamily, 0, &m_queue);

	// コンパニオンウィンドウの表示形式 (sRGB 変換しない UNORM を優先)
	uint32_t nFormatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &nFormatCount, NULL);
	std::vector<VkSurfaceFormatKHR> formats(nFormatCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &nFormatCount, formats.data());
	if (nFormatCount == 0)
	{
		return ShowError("No surface format is available.");
	}
	m_surfaceFormat = formats[0];
	for (const VkSurfaceFormatKHR& format : formats)
	{
		if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			m_surfaceFormat = format;
			break;
		}
	}

	// MSAA : 色と深度の両方が対応していれば 4x
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
	VkSampleCountFlags sampleCounts = properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;
	m_samples = (sampleCounts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;

	// Command buffer
	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = m_nQueueFamily;
	if (vkCreateCommandPool(m_device, &poolInfo, NULL, &m_commandPool) != VK_SUCCESS)
	{
		return ShowError("vkCreateCommandPool failed.");
	}
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffer) != VK_SUCCESS)
	{
		return ShowError("vkAllocateCommandBuffers failed.");
	}

	// Synchronization
	VkSemaphoreCreateInfo semaphoreInfo = {};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fenceInfo = {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if (vkCreateSemaphore(m_device, &semaphoreInfo, NULL, &m_imageAvailable) != VK_SUCCESS
		|| vkCreateFence(m_device, &fenceInfo, NULL, &m_inFlight) != VK_SUCCESS)
	{
		return ShowError("Failed to create synchronization objects.");
	}

	return true;
}

void CMdkvView::TermVulkan()
{
	if (m_device != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(m_device);

		if (m_pScene != NULL)
		{
			m_pScene->Term();
		}

		// OpenXR の swapchain はセッションより先に破棄する
		DeleteFrameBuffer(m_leftEyeDesc);
		DeleteFrameBuffer(m_rightEyeDesc);

		DestroySwapchain();
		vkDestroyFence(m_device, m_inFlight, NULL);
		vkDestroySemaphore(m_device, m_imageAvailable, NULL);
		vkDestroyCommandPool(m_device, m_commandPool, NULL);
		vkDestroyRenderPass(m_device, m_renderPass, NULL);
		m_inFlight = VK_NULL_HANDLE;
		m_imageAvailable = VK_NULL_HANDLE;
		m_commandBuffer = VK_NULL_HANDLE;
		m_commandPool = VK_NULL_HANDLE;
		m_renderPass = VK_NULL_HANDLE;
	}

	if (m_pScene != NULL)
	{
		delete m_pScene;
		m_pScene = NULL;
	}

	// セッションは Vulkan のデバイスを参照しているので、デバイスより先に破棄する
	TermVR();

	if (m_device != VK_NULL_HANDLE)
	{
		vkDestroyDevice(m_device, NULL);
		m_device = VK_NULL_HANDLE;
	}

	if (m_vkInstance != VK_NULL_HANDLE)
	{
		if (m_surface != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(m_vkInstance, m_surface, NULL);
		}
		vkDestroyInstance(m_vkInstance, NULL);
		m_surface = VK_NULL_HANDLE;
		m_vkInstance = VK_NULL_HANDLE;
	}

	if (m_instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
	}
}

bool CMdkvView::CreateRenderPass()
{
	// MSAA の色と深度に描き、resolve 先 (1 サンプル) を swapchain へのコピー元にする。
	// MSAA を使わないときは resolve 先に直接描く。
	const bool bMultisample = m_samples != VK_SAMPLE_COUNT_1_BIT;

	VkAttachmentDescription attachments[3] = {};
	// 0 : 色 (MSAA のときは MSAA バッファ、そうでなければ resolve 先)
	attachments[0].format = m_colorFormat;
	attachments[0].samples = m_samples;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = bMultisample ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = bMultisample ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	// 1 : 深度
	attachments[1].format = m_depthFormat;
	attachments[1].samples = m_samples;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	// 2 : resolve 先 (MSAA のときだけ)
	attachments[2].format = m_colorFormat;
	attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkAttachmentReference resolveRef = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;
	subpass.pResolveAttachments = bMultisample ? &resolveRef : NULL;
	subpass.pDepthStencilAttachment = &depthRef;

	VkSubpassDependency dependencies[2] = {};
	// 前のフレームの描画・コピーが終わってから書き込む
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	// 描き終えた resolve 先を、後のコピー (swapchain、コンパニオンウィンドウ) で読む
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	VkRenderPassCreateInfo renderPassInfo = {};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = bMultisample ? 3 : 2;
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 2;
	renderPassInfo.pDependencies = dependencies;
	if (vkCreateRenderPass(m_device, &renderPassInfo, NULL, &m_renderPass) != VK_SUCCESS)
	{
		return ShowError("vkCreateRenderPass failed.");
	}
	return true;
}

bool CMdkvView::CreateImage(VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect,
	VkImage* pImage, VkDeviceMemory* pMemory, VkImageView* pView)
{
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = format;
	imageInfo.extent = { m_nRenderWidth, m_nRenderHeight, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = samples;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = usage;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(m_device, &imageInfo, NULL, pImage) != VK_SUCCESS)
	{
		return false;
	}
	VkMemoryRequirements memReq;
	vkGetImageMemoryRequirements(m_device, *pImage, &memReq);
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(m_device, &allocInfo, NULL, pMemory) != VK_SUCCESS)
	{
		return false;
	}
	vkBindImageMemory(m_device, *pImage, *pMemory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.image = *pImage;
	viewInfo.format = format;
	viewInfo.subresourceRange = { aspect, 0, 1, 0, 1 };
	return vkCreateImageView(m_device, &viewInfo, NULL, pView) == VK_SUCCESS;
}

uint32_t CMdkvView::FindMemoryType(uint32_t nTypeBits, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if ((nTypeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}
	return 0;
}

//-----------------------------------------------------------------------------
// XR
//-----------------------------------------------------------------------------
bool CMdkvView::ShowXrError(XrResult result, const char* pszFunction)
{
	char szResult[XR_MAX_RESULT_STRING_SIZE] = "";
	if (m_instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(m_instance, result, szResult)))
	{
		sprintf_s(szResult, sizeof(szResult), "%d", (int)result);
	}

	char buf[1024];
	sprintf_s(buf, sizeof(buf), "%s failed: %s", pszFunction, szResult);
	::MessageBoxA(m_hWnd, buf, "OpenXR Error", MB_OK | MB_ICONERROR);
	return false;
}

bool CMdkvView::InitVR()
{
	XrResult result;

	// Vulkan 拡張がランタイムで使えるか確認する
	{
		uint32_t nCount = 0;
		result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &nCount, nullptr);
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrEnumerateInstanceExtensionProperties");
		}
		std::vector<XrExtensionProperties> extensions(nCount, { XR_TYPE_EXTENSION_PROPERTIES });
		result = xrEnumerateInstanceExtensionProperties(nullptr, nCount, &nCount, extensions.data());
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrEnumerateInstanceExtensionProperties");
		}
		bool bFound = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& ext) {
			return strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
		});
		if (!bFound)
		{
			return ShowError("The OpenXR runtime does not support " XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME ".");
		}
	}

	// Instance
	{
		const char* extensionNames[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
		XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
		createInfo.enabledExtensionCount = (uint32_t)_countof(extensionNames);
		createInfo.enabledExtensionNames = extensionNames;
		strcpy_s(createInfo.applicationInfo.applicationName, "mdkvr");
		createInfo.applicationInfo.applicationVersion = XR_MAKE_VERSION(0, 0, 3);
		createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
		result = xrCreateInstance(&createInfo, &m_instance);
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrCreateInstance");
		}
	}

	// System (HMD)
	{
		XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
		systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		result = xrGetSystem(m_instance, &systemInfo, &m_systemId);
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrGetSystem");
		}

		XrSystemProperties systemProperties{ XR_TYPE_SYSTEM_PROPERTIES };
		if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &systemProperties)))
		{
			std::string strTitle = std::string("MDKVR - ") + systemProperties.systemName;
			::SetWindowTextA(m_hWnd, strTitle.c_str());
		}
	}

	// Environment blend mode (ランタイムの優先順の先頭を使う)
	{
		uint32_t nCount = 0;
		result = xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, m_viewConfigType, 0, &nCount, nullptr);
		if (XR_FAILED(result) || nCount == 0)
		{
			return ShowXrError(result, "xrEnumerateEnvironmentBlendModes");
		}
		std::vector<XrEnvironmentBlendMode> blendModes(nCount);
		result = xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, m_viewConfigType, nCount, &nCount, blendModes.data());
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrEnumerateEnvironmentBlendModes");
		}
		m_blendMode = blendModes[0];
	}

	// Vulkan の要求バージョンを取得して、Vulkan を初期化する (xrCreateSession の前に必要)
	{
		PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetVulkanGraphicsRequirements2KHR = nullptr;
		result = xrGetInstanceProcAddr(m_instance, "xrGetVulkanGraphicsRequirements2KHR",
			reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVulkanGraphicsRequirements2KHR));
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrGetInstanceProcAddr");
		}

		XrGraphicsRequirementsVulkan2KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
		result = pfnGetVulkanGraphicsRequirements2KHR(m_instance, m_systemId, &graphicsRequirements);
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrGetVulkanGraphicsRequirements2KHR");
		}

		if (!InitVulkan(graphicsRequirements))
		{
			return false;
		}
	}

	// Session (Vulkan のデバイスとキューを渡す)
	{
		XrGraphicsBindingVulkan2KHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
		graphicsBinding.instance = m_vkInstance;
		graphicsBinding.physicalDevice = m_physicalDevice;
		graphicsBinding.device = m_device;
		graphicsBinding.queueFamilyIndex = m_nQueueFamily;
		graphicsBinding.queueIndex = 0;

		XrSessionCreateInfo createInfo{ XR_TYPE_SESSION_CREATE_INFO };
		createInfo.next = &graphicsBinding;
		createInfo.systemId = m_systemId;
		result = xrCreateSession(m_instance, &createInfo, &m_session);
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrCreateSession");
		}
	}

	// アプリの基準空間: 床が y=0 の STAGE を優先し、無ければ LOCAL を床付近まで下げて使う
	{
		uint32_t nCount = 0;
		xrEnumerateReferenceSpaces(m_session, 0, &nCount, nullptr);
		std::vector<XrReferenceSpaceType> spaces(nCount);
		xrEnumerateReferenceSpaces(m_session, nCount, &nCount, spaces.data());
		bool bStage = std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end();

		XrReferenceSpaceCreateInfo createInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		createInfo.poseInReferenceSpace.orientation.w = 1.0f;
		if (bStage)
		{
			createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
		}
		else
		{
			// LOCAL の原点は起動時の頭の位置なので、目の高さ分だけ下げる
			createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			createInfo.poseInReferenceSpace.position.y = -1.6f;
		}
		result = xrCreateReferenceSpace(m_session, &createInfo, &m_appSpace);
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrCreateReferenceSpace");
		}
	}

	// View configuration (両眼)
	{
		uint32_t nViewCount = 0;
		result = xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, 0, &nViewCount, nullptr);
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrEnumerateViewConfigurationViews");
		}
		m_configViews.assign(nViewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
		result = xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, nViewCount, &nViewCount, m_configViews.data());
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrEnumerateViewConfigurationViews");
		}
		if (nViewCount != Eye_Count)
		{
			return ShowError("Unexpected number of views.");
		}
		m_views.assign(nViewCount, { XR_TYPE_VIEW });
	}

	return true;
}

void CMdkvView::TermVR()
{
	if (m_appSpace != XR_NULL_HANDLE)
	{
		xrDestroySpace(m_appSpace);
		m_appSpace = XR_NULL_HANDLE;
	}
	if (m_session != XR_NULL_HANDLE)
	{
		xrDestroySession(m_session);
		m_session = XR_NULL_HANDLE;
	}
	m_bSessionRunning = false;
	// XrInstance は Vulkan のインスタンスより後に TermVulkan で破棄する
}

void CMdkvView::ProcessVREvents()
{
	XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
	{
		switch (event.type)
		{
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			// ランタイムが終了する
			::PostMessage(m_hWnd, WM_CLOSE, 0, 0);
			break;
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			const XrEventDataSessionStateChanged& stateChanged = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
			m_sessionState = stateChanged.state;
			switch (m_sessionState)
			{
			case XR_SESSION_STATE_READY:
			{
				XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType = m_viewConfigType;
				if (XR_SUCCEEDED(xrBeginSession(m_session, &beginInfo)))
				{
					m_bSessionRunning = true;
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				m_bSessionRunning = false;
				xrEndSession(m_session);
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				// ランタイム側から終了された
				::PostMessage(m_hWnd, WM_CLOSE, 0, 0);
				break;
			default:
				break;
			}
			break;
		}
		default:
			break;
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

bool CMdkvView::UpdateHMDMatrixPose(XrTime predictedDisplayTime)
{
	XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = m_viewConfigType;
	locateInfo.displayTime = predictedDisplayTime;
	locateInfo.space = m_appSpace;

	XrViewState viewState{ XR_TYPE_VIEW_STATE };
	uint32_t nViewCount = 0;
	XrResult result = xrLocateViews(m_session, &locateInfo, &viewState, (uint32_t)m_views.size(), &nViewCount, m_views.data());
	if (XR_FAILED(result) || nViewCount != Eye_Count)
	{
		return false;
	}
	if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
		(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
	{
		return false;	// トラッキングできていない
	}

	m_mat4ProjectionLeft = GetHMDMatrixProjectionEye(Eye_Left);
	m_mat4ProjectionRight = GetHMDMatrixProjectionEye(Eye_Right);
	m_mat4EyePosLeft = GetHMDMatrixPoseEye(Eye_Left);
	m_mat4EyePosRight = GetHMDMatrixPoseEye(Eye_Right);
	return true;
}

// 左右上下で非対称な視野角から Vulkan 用の射影行列 (深度 0..1、Y 軸下向き) を作る
glm::mat4 CMdkvView::GetHMDMatrixProjectionEye(EEye nEye)
{
	const XrFovf& fov = m_views[nEye].fov;
	glm::mat4 mat = glm::frustumRH_ZO(
		m_fNearClip * tanf(fov.angleLeft),
		m_fNearClip * tanf(fov.angleRight),
		m_fNearClip * tanf(fov.angleDown),
		m_fNearClip * tanf(fov.angleUp),
		m_fNearClip,
		m_fFarClip);
	// クリップ座標の Y を反転する (非対称な視野角では [1][1] だけでなく行全体を反転する)
	for (int col = 0; col < 4; col++)
	{
		mat[col][1] = -mat[col][1];
	}
	return mat;
}

// 目の姿勢 (アプリ空間) の逆行列 = view matrix
glm::mat4 CMdkvView::GetHMDMatrixPoseEye(EEye nEye)
{
	return glm::inverse(ConvertXrPose(m_views[nEye].pose));
}

glm::mat4 CMdkvView::GetCurrentViewMatrix(EEye nEye)
{
	return (nEye == Eye_Left) ? m_mat4EyePosLeft : m_mat4EyePosRight;
}

glm::mat4 CMdkvView::ConvertXrPose(const XrPosef& pose)
{
	const XrQuaternionf& q = pose.orientation;
	const XrVector3f& p = pose.position;
	glm::mat4 matRotation = glm::mat4_cast(glm::quat(q.w, q.x, q.y, q.z));
	return glm::translate(glm::mat4(1.0f), glm::vec3(p.x, p.y, p.z)) * matRotation;
}

//-----------------------------------------------------------------------------
// Render targets
//-----------------------------------------------------------------------------
bool CMdkvView::SetupStereoRenderTargets()
{
	// swapchain のフォーマットを選ぶ。
	// シーンはガンマ空間の値で描いているので、UNORM で描いた値を sRGB の swapchain にそのままコピーして
	// ランタイムに「sRGB エンコード済み」として扱わせる。vkCmdCopyImage は値を変換しないので、
	// 描画用の形式は swapchain と同じ並び (RGBA / BGRA) の UNORM にする。
	uint32_t nCount = 0;
	XrResult result = xrEnumerateSwapchainFormats(m_session, 0, &nCount, nullptr);
	if (XR_FAILED(result))
	{
		return ShowXrError(result, "xrEnumerateSwapchainFormats");
	}
	std::vector<int64_t> formats(nCount);
	result = xrEnumerateSwapchainFormats(m_session, nCount, &nCount, formats.data());
	if (XR_FAILED(result))
	{
		return ShowXrError(result, "xrEnumerateSwapchainFormats");
	}

	const struct { VkFormat swapchain; VkFormat render; } preferredFormats[] = {
		{ VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM },
		{ VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM },
		{ VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM },
		{ VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM },
	};
	m_nColorSwapchainFormat = 0;
	for (const auto& pair : preferredFormats)
	{
		if (std::find(formats.begin(), formats.end(), (int64_t)pair.swapchain) != formats.end())
		{
			m_nColorSwapchainFormat = pair.swapchain;
			m_colorFormat = pair.render;
			break;
		}
	}
	if (m_nColorSwapchainFormat == 0)
	{
		return ShowError("No supported swapchain format.");
	}

	// 左右で推奨サイズが違う場合に備えて大きい方に合わせる
	m_nRenderWidth = (std::max)(m_configViews[Eye_Left].recommendedImageRectWidth, m_configViews[Eye_Right].recommendedImageRectWidth);
	m_nRenderHeight = (std::max)(m_configViews[Eye_Left].recommendedImageRectHeight, m_configViews[Eye_Right].recommendedImageRectHeight);

	if (!CreateRenderPass())
	{
		return false;
	}
	if (!CreateFrameBuffer(m_nRenderWidth, m_nRenderHeight, m_leftEyeDesc))
	{
		return false;
	}
	if (!CreateFrameBuffer(m_nRenderWidth, m_nRenderHeight, m_rightEyeDesc))
	{
		return false;
	}
	return true;
}

bool CMdkvView::CreateFrameBuffer(int nWidth, int nHeight, FramebufferDesc& desc)
{
	const bool bMultisample = m_samples != VK_SAMPLE_COUNT_1_BIT;

	// MSAA 描画用 (resolve した後は不要なので TRANSIENT)
	if (bMultisample)
	{
		if (!CreateImage(m_colorFormat, m_samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT, &desc.m_renderImage, &desc.m_renderMemory, &desc.m_renderImageView))
		{
			return ShowError("Failed to create the MSAA color buffer.");
		}
	}
	if (!CreateImage(m_depthFormat, m_samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT, &desc.m_depthImage, &desc.m_depthMemory, &desc.m_depthImageView))
	{
		return ShowError("Failed to create the depth buffer.");
	}
	// resolve 先 (swapchain へのコピー元とコンパニオンウィンドウ表示)
	if (!CreateImage(m_colorFormat, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT, &desc.m_resolveImage, &desc.m_resolveMemory, &desc.m_resolveImageView))
	{
		return ShowError("Failed to create the resolve buffer.");
	}

	// CreateRenderPass のアタッチメントの並び (色, 深度, resolve 先) に合わせる
	VkImageView views[3];
	uint32_t nViewCount = 0;
	if (bMultisample)
	{
		views[0] = desc.m_renderImageView;
		views[1] = desc.m_depthImageView;
		views[2] = desc.m_resolveImageView;
		nViewCount = 3;
	}
	else
	{
		views[0] = desc.m_resolveImageView;
		views[1] = desc.m_depthImageView;
		nViewCount = 2;
	}
	VkFramebufferCreateInfo framebufferInfo = {};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass = m_renderPass;
	framebufferInfo.attachmentCount = nViewCount;
	framebufferInfo.pAttachments = views;
	framebufferInfo.width = nWidth;
	framebufferInfo.height = nHeight;
	framebufferInfo.layers = 1;
	if (vkCreateFramebuffer(m_device, &framebufferInfo, NULL, &desc.m_framebuffer) != VK_SUCCESS)
	{
		return ShowError("vkCreateFramebuffer failed.");
	}

	// OpenXR swapchain (resolve 先からコピーで書き込む)
	XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	createInfo.format = m_nColorSwapchainFormat;
	createInfo.sampleCount = 1;
	createInfo.width = nWidth;
	createInfo.height = nHeight;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;
	XrResult result = xrCreateSwapchain(m_session, &createInfo, &desc.m_swapchain);
	if (XR_FAILED(result))
	{
		return ShowXrError(result, "xrCreateSwapchain");
	}

	uint32_t nImageCount = 0;
	result = xrEnumerateSwapchainImages(desc.m_swapchain, 0, &nImageCount, nullptr);
	if (XR_FAILED(result))
	{
		return ShowXrError(result, "xrEnumerateSwapchainImages");
	}
	desc.m_swapchainImages.assign(nImageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
	result = xrEnumerateSwapchainImages(desc.m_swapchain, nImageCount, &nImageCount,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(desc.m_swapchainImages.data()));
	if (XR_FAILED(result))
	{
		return ShowXrError(result, "xrEnumerateSwapchainImages");
	}

	return true;
}

void CMdkvView::DeleteFrameBuffer(FramebufferDesc& desc)
{
	if (desc.m_swapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(desc.m_swapchain);
		desc.m_swapchain = XR_NULL_HANDLE;
	}
	desc.m_swapchainImages.clear();
	desc.m_bAcquired = false;

	vkDestroyFramebuffer(m_device, desc.m_framebuffer, NULL);
	vkDestroyImageView(m_device, desc.m_resolveImageView, NULL);
	vkDestroyImage(m_device, desc.m_resolveImage, NULL);
	vkFreeMemory(m_device, desc.m_resolveMemory, NULL);
	vkDestroyImageView(m_device, desc.m_renderImageView, NULL);
	vkDestroyImage(m_device, desc.m_renderImage, NULL);
	vkFreeMemory(m_device, desc.m_renderMemory, NULL);
	vkDestroyImageView(m_device, desc.m_depthImageView, NULL);
	vkDestroyImage(m_device, desc.m_depthImage, NULL);
	vkFreeMemory(m_device, desc.m_depthMemory, NULL);
	desc.m_framebuffer = VK_NULL_HANDLE;
	desc.m_resolveImageView = VK_NULL_HANDLE;
	desc.m_resolveImage = VK_NULL_HANDLE;
	desc.m_resolveMemory = VK_NULL_HANDLE;
	desc.m_renderImageView = VK_NULL_HANDLE;
	desc.m_renderImage = VK_NULL_HANDLE;
	desc.m_renderMemory = VK_NULL_HANDLE;
	desc.m_depthImageView = VK_NULL_HANDLE;
	desc.m_depthImage = VK_NULL_HANDLE;
	desc.m_depthMemory = VK_NULL_HANDLE;
}

// m_commandBuffer に 1 つの目の描画と swapchain へのコピーを記録する
bool CMdkvView::RenderEye(EEye nEye, FramebufferDesc& desc)
{
	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (XR_FAILED(xrAcquireSwapchainImage(desc.m_swapchain, &acquireInfo, &desc.m_nAcquiredIndex)))
	{
		return false;
	}
	desc.m_bAcquired = true;	// 失敗しても ReleaseEyeImages で返す

	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(xrWaitSwapchainImage(desc.m_swapchain, &waitInfo)))
	{
		return false;
	}

	const glm::mat4& matProjection = (nEye == Eye_Left) ? m_mat4ProjectionLeft : m_mat4ProjectionRight;
	VkExtent2D extent = { m_nRenderWidth, m_nRenderHeight };

	// シーンを描く (MSAA → resolve 先。resolve 先は TRANSFER_SRC_OPTIMAL になる)
	VkClearValue clearValues[3] = {};
	clearValues[0].color = { { 0.0f, 0.0f, 0.4f, 1.0f } };
	clearValues[1].depthStencil = { 1.0f, 0 };
	VkRenderPassBeginInfo renderPassBegin = {};
	renderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassBegin.renderPass = m_renderPass;
	renderPassBegin.framebuffer = desc.m_framebuffer;
	renderPassBegin.renderArea.extent = extent;
	renderPassBegin.clearValueCount = (m_samples != VK_SAMPLE_COUNT_1_BIT) ? 3 : 2;
	renderPassBegin.pClearValues = clearValues;
	vkCmdBeginRenderPass(m_commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);

	m_pScene->Draw(m_commandBuffer, extent, (uint32_t)nEye, matProjection, GetCurrentViewMatrix(nEye));

	vkCmdEndRenderPass(m_commandBuffer);

	// resolve 先 -> swapchain image (値をそのままコピーする)
	VkImage swapchainImage = desc.m_swapchainImages[desc.m_nAcquiredIndex].image;
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = swapchainImage;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	VkImageCopy region = {};
	region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.extent = { m_nRenderWidth, m_nRenderHeight, 1 };
	vkCmdCopyImage(m_commandBuffer, desc.m_resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// OpenXR には COLOR_ATTACHMENT_OPTIMAL の状態で返す
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	return true;
}

bool CMdkvView::RenderStereoTargets(std::vector<XrCompositionLayerProjectionView>& projectionViews)
{
	FramebufferDesc* pDescs[Eye_Count] = { &m_leftEyeDesc, &m_rightEyeDesc };

	projectionViews.assign(Eye_Count, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });
	for (int i = 0; i < Eye_Count; i++)
	{
		if (!RenderEye((EEye)i, *pDescs[i]))
		{
			return false;
		}

		// 描いたときの目の姿勢と視野角をランタイムに伝える
		projectionViews[i].pose = m_views[i].pose;
		projectionViews[i].fov = m_views[i].fov;
		projectionViews[i].subImage.swapchain = pDescs[i]->m_swapchain;
		projectionViews[i].subImage.imageRect.offset = { 0, 0 };
		projectionViews[i].subImage.imageRect.extent = { (int32_t)m_nRenderWidth, (int32_t)m_nRenderHeight };
	}
	m_bEyeRendered = true;
	return true;
}

// GPU にコマンドを投入した後で、取得した swapchain image を OpenXR に返す
void CMdkvView::ReleaseEyeImages()
{
	FramebufferDesc* pDescs[Eye_Count] = { &m_leftEyeDesc, &m_rightEyeDesc };
	for (FramebufferDesc* pDesc : pDescs)
	{
		if (pDesc->m_bAcquired)
		{
			XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(pDesc->m_swapchain, &releaseInfo);
			pDesc->m_bAcquired = false;
		}
	}
}

//-----------------------------------------------------------------------------
// Companion window
//-----------------------------------------------------------------------------
bool CMdkvView::SetupCompanionWindow()
{
	if (!CreateSwapchain())
	{
		DestroySwapchain();	// 最小化中などで作れなくても、次の Draw でもう一度作る
	}
	return true;
}

bool CMdkvView::CreateSwapchain()
{
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps);
	VkExtent2D extent = caps.currentExtent;
	if (extent.width == 0xFFFFFFFF)
	{
		RECT rcClient;
		::GetClientRect(m_hWnd, &rcClient);
		extent.width = (std::max)(caps.minImageExtent.width, (std::min)(caps.maxImageExtent.width, (uint32_t)(rcClient.right - rcClient.left)));
		extent.height = (std::max)(caps.minImageExtent.height, (std::min)(caps.maxImageExtent.height, (uint32_t)(rcClient.bottom - rcClient.top)));
	}
	if (extent.width == 0 || extent.height == 0)
	{
		return false;	// 最小化中は作れない
	}
	// 目の画像を vkCmdBlitImage で転送して表示する
	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
	{
		return false;
	}

	uint32_t nImageCount = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && nImageCount > caps.maxImageCount)
	{
		nImageCount = caps.maxImageCount;
	}
	VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (!(caps.supportedCompositeAlpha & compositeAlpha))
	{
		compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	}

	// フレームの同期は xrWaitFrame で取るので、表示で vsync を待たないモードを優先する
	uint32_t nModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &nModeCount, NULL);
	std::vector<VkPresentModeKHR> presentModes(nModeCount);
	vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &nModeCount, presentModes.data());
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
	for (VkPresentModeKHR mode : { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR })
	{
		if (std::find(presentModes.begin(), presentModes.end(), mode) != presentModes.end())
		{
			presentMode = mode;
			break;
		}
	}

	VkSwapchainCreateInfoKHR swapchainInfo = {};
	swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchainInfo.surface = m_surface;
	swapchainInfo.minImageCount = nImageCount;
	swapchainInfo.imageFormat = m_surfaceFormat.format;
	swapchainInfo.imageColorSpace = m_surfaceFormat.colorSpace;
	swapchainInfo.imageExtent = extent;
	swapchainInfo.imageArrayLayers = 1;
	swapchainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchainInfo.preTransform = caps.currentTransform;
	swapchainInfo.compositeAlpha = compositeAlpha;
	swapchainInfo.presentMode = presentMode;
	swapchainInfo.clipped = VK_TRUE;
	if (vkCreateSwapchainKHR(m_device, &swapchainInfo, NULL, &m_swapchain) != VK_SUCCESS)
	{
		m_swapchain = VK_NULL_HANDLE;
		return false;
	}
	m_extent = extent;

	uint32_t nCount = 0;
	vkGetSwapchainImagesKHR(m_device, m_swapchain, &nCount, NULL);
	m_swapchainImages.resize(nCount);
	vkGetSwapchainImagesKHR(m_device, m_swapchain, &nCount, m_swapchainImages.data());

	// スワップチェーンのイメージごとに描画完了のセマフォを作る
	VkSemaphoreCreateInfo semaphoreInfo = {};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	m_renderFinished.resize(nCount, VK_NULL_HANDLE);
	for (uint32_t i = 0; i < nCount; i++)
	{
		if (vkCreateSemaphore(m_device, &semaphoreInfo, NULL, &m_renderFinished[i]) != VK_SUCCESS)
		{
			return false;
		}
	}

	return true;
}

void CMdkvView::DestroySwapchain()
{
	for (VkSemaphore semaphore : m_renderFinished)
	{
		vkDestroySemaphore(m_device, semaphore, NULL);
	}
	m_renderFinished.clear();
	m_swapchainImages.clear();

	// 仕様では NULL でもよいが、NULL を渡すと落ちるレイヤー (Bandicam など) があるので呼ばない
	if (m_swapchain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(m_device, m_swapchain, NULL);
	}
	m_swapchain = VK_NULL_HANDLE;
}

void CMdkvView::RecreateSwapchain()
{
	vkDeviceWaitIdle(m_device);
	DestroySwapchain();
	if (!CreateSwapchain())
	{
		DestroySwapchain();	// 次の Draw でもう一度作る
	}
}

// m_commandBuffer に、左右の目の画像をウィンドウの左半分・右半分に転送するコマンドを記録する
void CMdkvView::RenderCompanionWindow(uint32_t nImageIndex)
{
	VkImage image = m_swapchainImages[nImageIndex];

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	if (m_bEyeRendered)
	{
		// resolve 先は描画後 TRANSFER_SRC_OPTIMAL のままになっている
		FramebufferDesc* pDescs[Eye_Count] = { &m_leftEyeDesc, &m_rightEyeDesc };
		int32_t nHalf = (int32_t)m_extent.width / 2;
		for (int i = 0; i < Eye_Count; i++)
		{
			VkImageBlit blit = {};
			blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.srcOffsets[0] = { 0, 0, 0 };
			blit.srcOffsets[1] = { (int32_t)m_nRenderWidth, (int32_t)m_nRenderHeight, 1 };
			blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.dstOffsets[0] = { i * nHalf, 0, 0 };
			blit.dstOffsets[1] = { (i == 0) ? nHalf : (int32_t)m_extent.width, (int32_t)m_extent.height, 1 };
			vkCmdBlitImage(m_commandBuffer, pDescs[i]->m_resolveImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
		}
	}
	else
	{
		// まだ一度も描いていないときは黒で塗る
		VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(m_commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	}

	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = 0;
	vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

void CMdkvView::Draw()
{
	if (m_device == VK_NULL_HANDLE || m_pScene == NULL || m_session == XR_NULL_HANDLE)
	{
		return;
	}

	ProcessVREvents();

	// OpenXR のフレームを始める
	XrFrameState frameState{ XR_TYPE_FRAME_STATE };
	bool bFrameBegun = false;
	if (m_bSessionRunning)
	{
		XrFrameWaitInfo frameWaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		if (XR_SUCCEEDED(xrWaitFrame(m_session, &frameWaitInfo, &frameState)))
		{
			XrFrameBeginInfo frameBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
			bFrameBegun = XR_SUCCEEDED(xrBeginFrame(m_session, &frameBeginInfo));
		}
	}
	else
	{
		// セッションが始まるまでは xrWaitFrame で待てないので、ループを間引く
		::Sleep(50);
	}

	// 前のフレームの GPU 処理が終わるのを待つ (コマンドバッファとユニフォームバッファを使い回すため)
	vkWaitForFences(m_device, 1, &m_inFlight, VK_TRUE, UINT64_MAX);

	// コンパニオンウィンドウのイメージを取得する
	if (m_swapchain == VK_NULL_HANDLE)
	{
		RecreateSwapchain();
	}
	uint32_t nCompanionIndex = 0;
	bool bCompanion = false;
	if (m_swapchain != VK_NULL_HANDLE)
	{
		VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_imageAvailable, VK_NULL_HANDLE, &nCompanionIndex);
		if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			RecreateSwapchain();
		}
		else if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
		{
			bCompanion = true;
		}
	}

	// 両目の描画とコンパニオンウィンドウへの転送を 1 つのコマンドバッファに記録する
	vkResetCommandBuffer(m_commandBuffer, 0);
	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(m_commandBuffer, &beginInfo);

	std::vector<XrCompositionLayerProjectionView> projectionViews;
	bool bLayer = false;
	if (bFrameBegun && frameState.shouldRender == XR_TRUE && UpdateHMDMatrixPose(frameState.predictedDisplayTime))
	{
		bLayer = RenderStereoTargets(projectionViews);
	}
	if (bCompanion)
	{
		RenderCompanionWindow(nCompanionIndex);
	}

	vkEndCommandBuffer(m_commandBuffer);

	vkResetFences(m_device, 1, &m_inFlight);
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = bCompanion ? 1 : 0;
	submitInfo.pWaitSemaphores = &m_imageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &m_commandBuffer;
	submitInfo.signalSemaphoreCount = bCompanion ? 1 : 0;
	submitInfo.pSignalSemaphores = bCompanion ? &m_renderFinished[nCompanionIndex] : NULL;
	vkQueueSubmit(m_queue, 1, &submitInfo, m_inFlight);

	// コマンドを投入したので、swapchain image を OpenXR に返す
	ReleaseEyeImages();

	if (bCompanion)
	{
		VkPresentInfoKHR presentInfo = {};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &m_renderFinished[nCompanionIndex];
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &m_swapchain;
		presentInfo.pImageIndices = &nCompanionIndex;
		VkResult result = vkQueuePresentKHR(m_queue, &presentInfo);
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		{
			RecreateSwapchain();
		}
	}

	// xrBeginFrame したら、描画しなかったフレームも xrEndFrame で閉じる
	if (bFrameBegun)
	{
		XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		std::vector<XrCompositionLayerBaseHeader*> layers;
		if (bLayer)
		{
			layer.space = m_appSpace;
			layer.viewCount = (uint32_t)projectionViews.size();
			layer.views = projectionViews.data();
			layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
		}
		XrFrameEndInfo frameEndInfo{ XR_TYPE_FRAME_END_INFO };
		frameEndInfo.displayTime = frameState.predictedDisplayTime;
		frameEndInfo.environmentBlendMode = m_blendMode;
		frameEndInfo.layerCount = (uint32_t)layers.size();
		frameEndInfo.layers = layers.data();
		xrEndFrame(m_session, &frameEndInfo);
	}
}
