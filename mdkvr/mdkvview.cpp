#include "pch.h"
#include "resource.h"
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <cstddef>
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

	std::string GetTrackedDeviceString(vr::IVRSystem* pHMD, vr::TrackedDeviceIndex_t unDevice, vr::TrackedDeviceProperty prop)
	{
		uint32_t unRequiredBufferLen = pHMD->GetStringTrackedDeviceProperty(unDevice, prop, NULL, 0, NULL);
		if (unRequiredBufferLen == 0)
		{
			return "";
		}
		std::string sResult(unRequiredBufferLen, '\0');
		pHMD->GetStringTrackedDeviceProperty(unDevice, prop, &sResult[0], unRequiredBufferLen, NULL);
		sResult.resize(unRequiredBufferLen - 1);
		return sResult;
	}
}

CMdkvView::CMdkvView() :
	m_hWnd(nullptr),
	m_hDC(nullptr),
	m_hRC(nullptr),
	m_pScene(nullptr),
	m_nWidth(800),
	m_nHeight(600),
	m_pHMD(nullptr),
	m_mat4HMDPose(1.0f),
	m_mat4EyePosLeft(1.0f),
	m_mat4EyePosRight(1.0f),
	m_mat4ProjectionLeft(1.0f),
	m_mat4ProjectionRight(1.0f),
	m_fNearClip(0.1f),
	m_fFarClip(30.0f),
	m_nRenderWidth(0),
	m_nRenderHeight(0),
	m_unCompanionProgramID(0),
	m_unCompanionVAO(0),
	m_glCompanionVertBuffer(0),
	m_glCompanionIndexBuffer(0),
	m_nCompanionIndexSize(0)
{
	memset(&m_leftEyeDesc, 0, sizeof(m_leftEyeDesc));
	memset(&m_rightEyeDesc, 0, sizeof(m_rightEyeDesc));
	memset(m_rTrackedDevicePose, 0, sizeof(m_rTrackedDevicePose));
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
	TermVR();

	if (m_hRC != NULL)
	{
		if (m_pScene != NULL)
		{
			m_pScene->Term();
		}

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
// VR
//-----------------------------------------------------------------------------
bool CMdkvView::InitVR()
{
	// Loading the SteamVR Runtime
	vr::EVRInitError eError = vr::VRInitError_None;
	m_pHMD = vr::VR_Init(&eError, vr::VRApplication_Scene);
	if (eError != vr::VRInitError_None)
	{
		m_pHMD = NULL;
		char buf[1024];
		sprintf_s(buf, sizeof(buf), "Unable to init VR runtime: %s", vr::VR_GetVRInitErrorAsEnglishDescription(eError));
		::MessageBoxA(m_hWnd, buf, "VR_Init Failed", MB_OK | MB_ICONERROR);
		return false;
	}

	if (!vr::VRCompositor())
	{
		::MessageBoxA(m_hWnd, "Compositor initialization failed. See log file for details", "VR_Init Failed", MB_OK | MB_ICONERROR);
		return false;
	}

	std::string strDriver = GetTrackedDeviceString(m_pHMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String);
	std::string strDisplay = GetTrackedDeviceString(m_pHMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SerialNumber_String);
	std::string strTitle = "MDKVR - " + strDriver + " " + strDisplay;
	::SetWindowTextA(m_hWnd, strTitle.c_str());

	// Cameras
	m_mat4ProjectionLeft = GetHMDMatrixProjectionEye(vr::Eye_Left);
	m_mat4ProjectionRight = GetHMDMatrixProjectionEye(vr::Eye_Right);
	m_mat4EyePosLeft = GetHMDMatrixPoseEye(vr::Eye_Left);
	m_mat4EyePosRight = GetHMDMatrixPoseEye(vr::Eye_Right);

	return true;
}

void CMdkvView::TermVR()
{
	if (m_pHMD != NULL)
	{
		vr::VR_Shutdown();
		m_pHMD = NULL;
	}
}

void CMdkvView::ProcessVREvents()
{
	vr::VREvent_t event;
	while (m_pHMD->PollNextEvent(&event, sizeof(event)))
	{
		switch (event.eventType)
		{
		case vr::VREvent_Quit:
			// SteamVR からの終了要求
			vr::VRSystem()->AcknowledgeQuit_Exiting();
			::PostMessage(m_hWnd, WM_CLOSE, 0, 0);
			break;
		case vr::VREvent_TrackedDeviceDeactivated:
		case vr::VREvent_TrackedDeviceUpdated:
		default:
			break;
		}
	}
}

void CMdkvView::UpdateHMDMatrixPose()
{
	vr::VRCompositor()->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, NULL, 0);

	if (m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
	{
		m_mat4HMDPose = glm::inverse(ConvertSteamVRMatrix(m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking));
	}
}

glm::mat4 CMdkvView::GetHMDMatrixProjectionEye(vr::Hmd_Eye nEye)
{
	return ConvertSteamVRMatrix(m_pHMD->GetProjectionMatrix(nEye, m_fNearClip, m_fFarClip));
}

glm::mat4 CMdkvView::GetHMDMatrixPoseEye(vr::Hmd_Eye nEye)
{
	return glm::inverse(ConvertSteamVRMatrix(m_pHMD->GetEyeToHeadTransform(nEye)));
}

glm::mat4 CMdkvView::GetCurrentViewMatrix(vr::Hmd_Eye nEye)
{
	if (nEye == vr::Eye_Left)
	{
		return m_mat4EyePosLeft * m_mat4HMDPose;
	}
	return m_mat4EyePosRight * m_mat4HMDPose;
}

// OpenVR の行列は行優先 (m[row][col])、glm は列優先 (mat[col][row])
glm::mat4 CMdkvView::ConvertSteamVRMatrix(const vr::HmdMatrix34_t& matPose)
{
	glm::mat4 mat(1.0f);
	for (int row = 0; row < 3; row++)
	{
		for (int col = 0; col < 4; col++)
		{
			mat[col][row] = matPose.m[row][col];
		}
	}
	return mat;
}

glm::mat4 CMdkvView::ConvertSteamVRMatrix(const vr::HmdMatrix44_t& matSrc)
{
	glm::mat4 mat(1.0f);
	for (int row = 0; row < 4; row++)
	{
		for (int col = 0; col < 4; col++)
		{
			mat[col][row] = matSrc.m[row][col];
		}
	}
	return mat;
}

//-----------------------------------------------------------------------------
// Render targets
//-----------------------------------------------------------------------------
bool CMdkvView::SetupStereoRenderTargets()
{
	m_pHMD->GetRecommendedRenderTargetSize(&m_nRenderWidth, &m_nRenderHeight);

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

	// Compositor へ渡す resolve 用
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

	return status == GL_FRAMEBUFFER_COMPLETE;
}

void CMdkvView::DeleteFrameBuffer(FramebufferDesc& desc)
{
	glDeleteRenderbuffers(1, &desc.m_nDepthBufferId);
	glDeleteTextures(1, &desc.m_nRenderTextureId);
	glDeleteFramebuffers(1, &desc.m_nRenderFramebufferId);
	glDeleteTextures(1, &desc.m_nResolveTextureId);
	glDeleteFramebuffers(1, &desc.m_nResolveFramebufferId);
	memset(&desc, 0, sizeof(desc));
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

void CMdkvView::RenderEye(vr::Hmd_Eye nEye, FramebufferDesc& desc)
{
	const glm::mat4& matProjection = (nEye == vr::Eye_Left) ? m_mat4ProjectionLeft : m_mat4ProjectionRight;

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
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

void CMdkvView::RenderStereoTargets()
{
	RenderEye(vr::Eye_Left, m_leftEyeDesc);
	RenderEye(vr::Eye_Right, m_rightEyeDesc);
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

	FramebufferDesc* pDescs[2] = { &m_leftEyeDesc, &m_rightEyeDesc };
	for (int i = 0; i < 2; i++)
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
	if (m_pScene == NULL || m_hDC == NULL || m_pHMD == NULL)
	{
		return;
	}

	ProcessVREvents();

	RenderStereoTargets();
	RenderCompanionWindow();

	vr::Texture_t leftEyeTexture = { (void*)(uintptr_t)m_leftEyeDesc.m_nResolveTextureId, vr::TextureType_OpenGL, vr::ColorSpace_Gamma };
	vr::VRCompositor()->Submit(vr::Eye_Left, &leftEyeTexture);
	vr::Texture_t rightEyeTexture = { (void*)(uintptr_t)m_rightEyeDesc.m_nResolveTextureId, vr::TextureType_OpenGL, vr::ColorSpace_Gamma };
	vr::VRCompositor()->Submit(vr::Eye_Right, &rightEyeTexture);

	::SwapBuffers(m_hDC);

	UpdateHMDMatrixPose();
}
