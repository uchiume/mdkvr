#include "pch.h"
#include "resource.h"
#include <GL/glew.h>
#include <GL/wglew.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <string>
#include <cstddef>
#include <cmath>
#include "mdkvscene.h"
#include "mdkvview.h"

namespace
{
	// コンパニオンウィンドウ用頂点 (位置 xy, UV)
	struct VertexDataWindow
	{
		glm::vec2 position;
		glm::vec2 texCoord;
	};
}

CMdkvView::CMdkvView() :
	m_hWnd(nullptr),
	m_hDC(nullptr),
	m_hRC(nullptr),
	m_pScene(nullptr),
	m_nWidth(800),
	m_nHeight(600),
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
	m_rightEyeDesc(),
	m_unCompanionProgramID(0),
	m_unCompanionVAO(0),
	m_glCompanionVertBuffer(0),
	m_glCompanionIndexBuffer(0),
	m_nCompanionIndexSize(0)
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
	case WM_SIZE:
		thisPtr->m_nWidth = LOWORD(lParam);
		thisPtr->m_nHeight = HIWORD(lParam);
		break;
	case WM_CLOSE:
		::DestroyWindow(hWnd);
		break;
	case WM_DESTROY:
		thisPtr->TermGL();
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

	if (!InitGL())
	{
		::DestroyWindow(m_hWnd);
		m_hWnd = NULL;
		return false;
	}

	::ShowWindow(m_hWnd, SW_SHOW);

	return true;
}

bool CMdkvView::InitGL()
{
	m_hDC = ::GetDC(m_hWnd);
	if (m_hDC == NULL)
	{
		return false;
	}

	PIXELFORMATDESCRIPTOR pfd;
	memset(&pfd, 0, sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;
	pfd.cStencilBits = 8;
	pfd.iLayerType = PFD_MAIN_PLANE;

	int nPixelFormat = ::ChoosePixelFormat(m_hDC, &pfd);
	if (nPixelFormat == 0)
	{
		return false;
	}

	if (!::SetPixelFormat(m_hDC, nPixelFormat, &pfd))
	{
		return false;
	}

	m_hRC = ::wglCreateContext(m_hDC);
	if (m_hRC == NULL)
	{
		return false;
	}

	if (!::wglMakeCurrent(m_hDC, m_hRC))
	{
		return false;
	}

	glewExperimental = GL_TRUE;
	if (::glewInit() != GLEW_OK)
	{
		return false;
	}
	glGetError(); // to clear the error caused deep in GLEW

	// フレームの同期は xrWaitFrame で取るので、コンパニオンウィンドウの SwapBuffers は vsync を待たない
	if (WGLEW_EXT_swap_control)
	{
		wglSwapIntervalEXT(0);
	}

	RECT rcClient;
	::GetClientRect(m_hWnd, &rcClient);
	m_nWidth = rcClient.right - rcClient.left;
	m_nHeight = rcClient.bottom - rcClient.top;

	if (!InitVR())
	{
		return false;
	}

	m_pScene = new CMdkvScene();
	if (!m_pScene->Init())
	{
		return false;
	}

	if (!SetupStereoRenderTargets())
	{
		return false;
	}

	if (!SetupCompanionWindow())
	{
		return false;
	}

	return true;
}

void CMdkvView::TermGL()
{
	if (m_hRC != NULL)
	{
		if (m_pScene != NULL)
		{
			m_pScene->Term();
		}

		// swapchain はセッションより先に破棄する
		DeleteFrameBuffer(m_leftEyeDesc);
		DeleteFrameBuffer(m_rightEyeDesc);

		if (m_unCompanionVAO != 0)
		{
			glDeleteVertexArrays(1, &m_unCompanionVAO);
			m_unCompanionVAO = 0;
		}
		if (m_glCompanionVertBuffer != 0)
		{
			glDeleteBuffers(1, &m_glCompanionVertBuffer);
			m_glCompanionVertBuffer = 0;
		}
		if (m_glCompanionIndexBuffer != 0)
		{
			glDeleteBuffers(1, &m_glCompanionIndexBuffer);
			m_glCompanionIndexBuffer = 0;
		}
		if (m_unCompanionProgramID != 0)
		{
			glDeleteProgram(m_unCompanionProgramID);
			m_unCompanionProgramID = 0;
		}
	}

	// セッションは GL コンテキストを参照しているので、コンテキストより先に破棄する
	TermVR();

	if (m_pScene != NULL)
	{
		delete m_pScene;
		m_pScene = NULL;
	}

	if (m_hRC != NULL)
	{
		::wglMakeCurrent(NULL, NULL);
		::wglDeleteContext(m_hRC);
		m_hRC = NULL;
	}

	if (m_hDC != NULL)
	{
		::ReleaseDC(m_hWnd, m_hDC);
		m_hDC = NULL;
	}
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

	// OpenGL 拡張がランタイムで使えるか確認する
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
			return strcmp(ext.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0;
		});
		if (!bFound)
		{
			::MessageBoxA(m_hWnd, "The OpenXR runtime does not support " XR_KHR_OPENGL_ENABLE_EXTENSION_NAME ".", "OpenXR Error", MB_OK | MB_ICONERROR);
			return false;
		}
	}

	// Instance
	{
		const char* extensionNames[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
		XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
		createInfo.enabledExtensionCount = (uint32_t)_countof(extensionNames);
		createInfo.enabledExtensionNames = extensionNames;
		strcpy_s(createInfo.applicationInfo.applicationName, "mdkvr");
		createInfo.applicationInfo.applicationVersion = XR_MAKE_VERSION(0, 0, 2);
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

	// OpenGL の要求バージョンを確認する (xrCreateSession の前に呼ぶ必要がある)
	{
		PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetOpenGLGraphicsRequirementsKHR = nullptr;
		result = xrGetInstanceProcAddr(m_instance, "xrGetOpenGLGraphicsRequirementsKHR",
			reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetOpenGLGraphicsRequirementsKHR));
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrGetInstanceProcAddr");
		}

		XrGraphicsRequirementsOpenGLKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
		result = pfnGetOpenGLGraphicsRequirementsKHR(m_instance, m_systemId, &graphicsRequirements);
		if (XR_FAILED(result))
		{
			return ShowXrError(result, "xrGetOpenGLGraphicsRequirementsKHR");
		}

		GLint nMajor = 0;
		GLint nMinor = 0;
		glGetIntegerv(GL_MAJOR_VERSION, &nMajor);
		glGetIntegerv(GL_MINOR_VERSION, &nMinor);
		if (graphicsRequirements.minApiVersionSupported > XR_MAKE_VERSION(nMajor, nMinor, 0))
		{
			::MessageBoxA(m_hWnd, "The OpenXR runtime requires a newer OpenGL version.", "OpenXR Error", MB_OK | MB_ICONERROR);
			return false;
		}
	}

	// Session (このウィンドウの GL コンテキストを渡す)
	{
		XrGraphicsBindingOpenGLWin32KHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
		graphicsBinding.hDC = m_hDC;
		graphicsBinding.hGLRC = m_hRC;

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
			::MessageBoxA(m_hWnd, "Unexpected number of views.", "OpenXR Error", MB_OK | MB_ICONERROR);
			return false;
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
	if (m_instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
	}
	m_bSessionRunning = false;
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

// 左右上下で非対称な視野角から OpenGL の射影行列を作る
glm::mat4 CMdkvView::GetHMDMatrixProjectionEye(EEye nEye)
{
	const XrFovf& fov = m_views[nEye].fov;
	return glm::frustum(
		m_fNearClip * tanf(fov.angleLeft),
		m_fNearClip * tanf(fov.angleRight),
		m_fNearClip * tanf(fov.angleDown),
		m_fNearClip * tanf(fov.angleUp),
		m_fNearClip,
		m_fFarClip);
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
	// シーンはガンマ空間の値で描いているので、sRGB フォーマットにそのままコピーして
	// ランタイムに「sRGB エンコード済み」として扱わせる。無ければ RGBA8 を使う。
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

	const int64_t preferredFormats[] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
	m_nColorSwapchainFormat = 0;
	for (int64_t format : preferredFormats)
	{
		if (std::find(formats.begin(), formats.end(), format) != formats.end())
		{
			m_nColorSwapchainFormat = format;
			break;
		}
	}
	if (m_nColorSwapchainFormat == 0)
	{
		::MessageBoxA(m_hWnd, "No supported swapchain format.", "OpenXR Error", MB_OK | MB_ICONERROR);
		return false;
	}

	// 左右で推奨サイズが違う場合に備えて大きい方に合わせる
	m_nRenderWidth = (std::max)(m_configViews[Eye_Left].recommendedImageRectWidth, m_configViews[Eye_Right].recommendedImageRectWidth);
	m_nRenderHeight = (std::max)(m_configViews[Eye_Left].recommendedImageRectHeight, m_configViews[Eye_Right].recommendedImageRectHeight);

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
	// MSAA 描画用
	glGenFramebuffers(1, &desc.m_nRenderFramebufferId);
	glBindFramebuffer(GL_FRAMEBUFFER, desc.m_nRenderFramebufferId);

	glGenRenderbuffers(1, &desc.m_nDepthBufferId);
	glBindRenderbuffer(GL_RENDERBUFFER, desc.m_nDepthBufferId);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT, nWidth, nHeight);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, desc.m_nDepthBufferId);

	glGenTextures(1, &desc.m_nRenderTextureId);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, desc.m_nRenderTextureId);
	glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, nWidth, nHeight, true);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, desc.m_nRenderTextureId, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return false;
	}

	// resolve 用 (コンパニオンウィンドウ表示と swapchain へのコピー元)
	glGenFramebuffers(1, &desc.m_nResolveFramebufferId);
	glBindFramebuffer(GL_FRAMEBUFFER, desc.m_nResolveFramebufferId);

	glGenTextures(1, &desc.m_nResolveTextureId);
	glBindTexture(GL_TEXTURE_2D, desc.m_nResolveTextureId);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nWidth, nHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, desc.m_nResolveTextureId, 0);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		return false;
	}

	// swapchain image を貼り付ける FBO (毎フレーム取得した image を attach する)
	glGenFramebuffers(1, &desc.m_nSwapchainFramebufferId);

	// OpenXR swapchain
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
	desc.m_swapchainImages.assign(nImageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
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

	glDeleteRenderbuffers(1, &desc.m_nDepthBufferId);
	glDeleteTextures(1, &desc.m_nRenderTextureId);
	glDeleteFramebuffers(1, &desc.m_nRenderFramebufferId);
	glDeleteTextures(1, &desc.m_nResolveTextureId);
	glDeleteFramebuffers(1, &desc.m_nResolveFramebufferId);
	glDeleteFramebuffers(1, &desc.m_nSwapchainFramebufferId);
	desc.m_nDepthBufferId = 0;
	desc.m_nRenderTextureId = 0;
	desc.m_nRenderFramebufferId = 0;
	desc.m_nResolveTextureId = 0;
	desc.m_nResolveFramebufferId = 0;
	desc.m_nSwapchainFramebufferId = 0;
}

bool CMdkvView::SetupCompanionWindow()
{
	m_unCompanionProgramID = CMdkvScene::CreateProgram(IDR_COMPANION_VSHADER, IDR_COMPANION_FSHADER);
	if (m_unCompanionProgramID == 0)
	{
		return false;
	}

	// 左半分に左目、右半分に右目
	VertexDataWindow vVerts[] = {
		// left eye
		{ glm::vec2(-1, -1), glm::vec2(0, 0) },
		{ glm::vec2( 0, -1), glm::vec2(1, 0) },
		{ glm::vec2(-1,  1), glm::vec2(0, 1) },
		{ glm::vec2( 0,  1), glm::vec2(1, 1) },
		// right eye
		{ glm::vec2( 0, -1), glm::vec2(0, 0) },
		{ glm::vec2( 1, -1), glm::vec2(1, 0) },
		{ glm::vec2( 0,  1), glm::vec2(0, 1) },
		{ glm::vec2( 1,  1), glm::vec2(1, 1) },
	};
	GLushort vIndices[] = { 0, 1, 3,   0, 3, 2,   4, 5, 7,   4, 7, 6 };
	m_nCompanionIndexSize = (GLsizei)_countof(vIndices);

	glGenVertexArrays(1, &m_unCompanionVAO);
	glBindVertexArray(m_unCompanionVAO);

	glGenBuffers(1, &m_glCompanionVertBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, m_glCompanionVertBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vVerts), vVerts, GL_STATIC_DRAW);

	glGenBuffers(1, &m_glCompanionIndexBuffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_glCompanionIndexBuffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(vIndices), vIndices, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataWindow), (void*)offsetof(VertexDataWindow, position));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataWindow), (void*)offsetof(VertexDataWindow, texCoord));

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	return true;
}

bool CMdkvView::RenderEye(EEye nEye, FramebufferDesc& desc)
{
	const glm::mat4& matProjection = (nEye == Eye_Left) ? m_mat4ProjectionLeft : m_mat4ProjectionRight;

	// シーンを MSAA バッファに描く
	glEnable(GL_MULTISAMPLE);

	glBindFramebuffer(GL_FRAMEBUFFER, desc.m_nRenderFramebufferId);
	glViewport(0, 0, m_nRenderWidth, m_nRenderHeight);
	glClearColor(0.0f, 0.0f, 0.4f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);

	m_pScene->Draw(matProjection, GetCurrentViewMatrix(nEye));

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	glDisable(GL_MULTISAMPLE);

	// MSAA -> resolve
	glBindFramebuffer(GL_READ_FRAMEBUFFER, desc.m_nRenderFramebufferId);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, desc.m_nResolveFramebufferId);
	glBlitFramebuffer(0, 0, m_nRenderWidth, m_nRenderHeight, 0, 0, m_nRenderWidth, m_nRenderHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);

	// resolve -> swapchain image
	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t nImageIndex = 0;
	if (XR_FAILED(xrAcquireSwapchainImage(desc.m_swapchain, &acquireInfo, &nImageIndex)))
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return false;
	}

	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	bool bWaited = XR_SUCCEEDED(xrWaitSwapchainImage(desc.m_swapchain, &waitInfo));
	if (bWaited)
	{
		// GL_FRAMEBUFFER_SRGB は無効のままなので、sRGB の swapchain にも値がそのままコピーされる
		glBindFramebuffer(GL_READ_FRAMEBUFFER, desc.m_nResolveFramebufferId);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, desc.m_nSwapchainFramebufferId);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, desc.m_swapchainImages[nImageIndex].image, 0);
		glBlitFramebuffer(0, 0, m_nRenderWidth, m_nRenderHeight, 0, 0, m_nRenderWidth, m_nRenderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	// acquire した image は必ず release する
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(desc.m_swapchain, &releaseInfo);

	return bWaited;
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
	return true;
}

void CMdkvView::RenderCompanionWindow()
{
	glDisable(GL_DEPTH_TEST);
	glViewport(0, 0, m_nWidth, m_nHeight);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glBindVertexArray(m_unCompanionVAO);
	glUseProgram(m_unCompanionProgramID);
	glActiveTexture(GL_TEXTURE0);

	FramebufferDesc* pDescs[Eye_Count] = { &m_leftEyeDesc, &m_rightEyeDesc };
	for (int i = 0; i < Eye_Count; i++)
	{
		glBindTexture(GL_TEXTURE_2D, pDescs[i]->m_nResolveTextureId);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		// インデックス配列の前半が左目、後半が右目
		glDrawElements(GL_TRIANGLES, m_nCompanionIndexSize / 2, GL_UNSIGNED_SHORT,
			(const void*)(uintptr_t)(i * (m_nCompanionIndexSize / 2) * sizeof(GLushort)));
	}

	glBindVertexArray(0);
	glUseProgram(0);
}

void CMdkvView::Draw()
{
	if (m_pScene == NULL || m_hDC == NULL || m_session == XR_NULL_HANDLE)
	{
		return;
	}

	ProcessVREvents();

	if (m_bSessionRunning)
	{
		XrFrameWaitInfo frameWaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState frameState{ XR_TYPE_FRAME_STATE };
		if (XR_FAILED(xrWaitFrame(m_session, &frameWaitInfo, &frameState)))
		{
			return;
		}

		XrFrameBeginInfo frameBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		if (XR_FAILED(xrBeginFrame(m_session, &frameBeginInfo)))
		{
			return;
		}

		std::vector<XrCompositionLayerBaseHeader*> layers;
		XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		std::vector<XrCompositionLayerProjectionView> projectionViews;
		if (frameState.shouldRender == XR_TRUE &&
			UpdateHMDMatrixPose(frameState.predictedDisplayTime) &&
			RenderStereoTargets(projectionViews))
		{
			layer.space = m_appSpace;
			layer.viewCount = (uint32_t)projectionViews.size();
			layer.views = projectionViews.data();
			layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
		}

		// xrBeginFrame したら、描画しなかったフレームも xrEndFrame で閉じる
		XrFrameEndInfo frameEndInfo{ XR_TYPE_FRAME_END_INFO };
		frameEndInfo.displayTime = frameState.predictedDisplayTime;
		frameEndInfo.environmentBlendMode = m_blendMode;
		frameEndInfo.layerCount = (uint32_t)layers.size();
		frameEndInfo.layers = layers.data();
		xrEndFrame(m_session, &frameEndInfo);
	}
	else
	{
		// セッションが始まるまでは xrWaitFrame で待てないので、ループを間引く
		::Sleep(50);
	}

	RenderCompanionWindow();
	::SwapBuffers(m_hDC);
}
