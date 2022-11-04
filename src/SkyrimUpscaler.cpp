#include <SkyrimUpscaler.h>
#include <PCH.h>
#include <DRS.h>
#include <ReShadePlugin.h>


#define GetSettingInt(a_section, a_setting, a_default) a_setting = (int)ini.GetLongValue(a_section, #a_setting, a_default);
#define SetSettingInt(a_section, a_setting) ini.SetLongValue(a_section, #a_setting, a_setting);

#define GetSettingFloat(a_section, a_setting, a_default) a_setting = (float)ini.GetDoubleValue(a_section, #a_setting, a_default);
#define SetSettingFloat(a_section, a_setting) ini.SetDoubleValue(a_section, #a_setting, a_setting);

#define GetSettingBool(a_section, a_setting, a_default) a_setting = ini.GetBoolValue(a_section, #a_setting, a_default);
#define SetSettingBool(a_section, a_setting) ini.SetBoolValue(a_section, #a_setting, a_setting);

void SkyrimUpscaler::LoadINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(L"Data\\SKSE\\Plugins\\SkyrimUpscaler.ini");
	GetSettingBool("Settings", mEnableUpscaler, false);
	GetSettingInt("Settings", mUpscaleType, 0);
	GetSettingInt("Settings", mQualityLevel, 0);
}
void SkyrimUpscaler::SaveINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(L"Data\\SKSE\\Plugins\\SkyrimUpscaler.ini");
	SetSettingBool("Settings", mEnableUpscaler);
	SetSettingInt("Settings", mUpscaleType);
	SetSettingInt("Settings", mQualityLevel);
}

void SkyrimUpscaler::MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
	case SKSE::MessagingInterface::kNewGame:
	case SKSE::MessagingInterface::kPreLoadGame:
		LoadINI();
		break;

	case SKSE::MessagingInterface::kSaveGame:
	case SKSE::MessagingInterface::kDeleteGame:
		SaveINI();
		break;
	}
}

void SkyrimUpscaler::SetupSwapChain(IDXGISwapChain* swapchain)
{
	mSwapChain = swapchain;
	mSwapChain->GetDevice(IID_PPV_ARGS(&mD3d11Device));
	SetupDirectX(mD3d11Device, 0);
}

float SkyrimUpscaler::GetVerticalFOVRad()
{
	static float& fac = (*(float*)(RELOCATION_ID(513786, 388785).address()));
	const auto    base = fac;
	const auto    x = base / 1.30322540f;
	const auto    vFOV = 2 * atan(x / (float(mRenderSizeX) / mRenderSizeY));
	return vFOV;
}

void SkyrimUpscaler::EvaluateUpscaler()
{
	static const float Deg2Rad = 0.01745329252f;
	static const float Rad2Deg = 57.29578f;
	static float&      g_fNear = (*(float*)(RELOCATION_ID(517032, 403540).address() + 0x40));  // 2F26FC0, 2FC1A90
	static float&      g_fFar = (*(float*)(RELOCATION_ID(517032, 403540).address() + 0x44));   // 2F26FC4, 2FC1A94

	if (mSwapChain != nullptr) {
		ID3D11Texture2D* back_buffer;
		mSwapChain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
		if (back_buffer != nullptr && mDepthBuffer != nullptr && mMotionVectors != nullptr) {
			ID3D11Texture2D* motionVectorTex = (RE::UI::GetSingleton()->GameIsPaused() ? mMotionVectorsEmpty : mMotionVectors);
			if (mEnableUpscaler && !DRS::GetSingleton()->reset) {
				ID3D11DeviceContext* context;
				mD3d11Device->GetImmediateContext(&context);
				// For DLSS to work in borderless mode we must copy the backbuffer to a temporally texture
				context->CopyResource(mTempColor, back_buffer);
				int j = (mEnableJitter) ? 1 : 0;
				if (!mDisableEvaluation)
					EvaluateUpscale(0, mTempColor, motionVectorTex, mDepthBuffer, nullptr, mSharpness, mJitterOffsets[0][0] * j, mJitterOffsets[0][1] * j, false, g_fNear / 100, g_fFar / 100, GetVerticalFOVRad());
				if (!mDisableResultCopying)
					context->CopyResource(back_buffer, mOutColor);
			}
		}
	}
}

void SkyrimUpscaler::GetJitters(float* out_x, float* out_y)
{
	const auto phase = GetJitterPhaseCount(0);

	mJitterIndex[0]++;
	GetJitterOffset(out_x, out_y, mJitterIndex[0], phase);
}

void SkyrimUpscaler::SetJitterOffsets(float x, float y)
{
	mJitterOffsets[0][0] = x;
	mJitterOffsets[0][1] = y;
}

void SkyrimUpscaler::SetMotionScale(float x, float y)
{
	mMotionScale[0] = x;
	mMotionScale[1] = y;
	SetMotionScaleX(0, x);
	SetMotionScaleX(0, y);
}

void SkyrimUpscaler::SetEnabled(bool enabled)
{
	mEnableUpscaler = enabled;
	if (mEnableUpscaler) {
		DRS::GetSingleton()->targetScaleFactor = mRenderScale;
		DRS::GetSingleton()->ControlResolution();
		mMipLodBias = GetOptimalMipmapBias(0);
	} else {
		DRS::GetSingleton()->targetScaleFactor = 1.0f;
		DRS::GetSingleton()->ControlResolution();
		mMipLodBias = 0;
	}
}

void SkyrimUpscaler::SetupDepth(ID3D11Texture2D* depth_buffer)
{
	mDepthBuffer = depth_buffer;
}

void SkyrimUpscaler::SetupMotionVector(ID3D11Texture2D* motion_buffer)
{
	mMotionVectors = motion_buffer;
	if (!mMotionVectorsEmpty) {
		D3D11_TEXTURE2D_DESC desc;
		motion_buffer->GetDesc(&desc);
		mD3d11Device->CreateTexture2D(&desc, NULL, &mMotionVectorsEmpty);
	}
}

void SkyrimUpscaler::InitUpscaler()
{
	ID3D11Texture2D* back_buffer;
	mSwapChain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
	D3D11_TEXTURE2D_DESC desc;
	back_buffer->GetDesc(&desc);
	mDisplaySizeX = desc.Width;
	mDisplaySizeY = desc.Height;
	mOutColor = (ID3D11Texture2D*)InitUpscaleFeature(0, mUpscaleType, mQualityLevel, desc.Width, desc.Height, false, false, false, false, mSharpening, true, desc.Format);
	if (mOutColor == nullptr) {
		mEnableUpscaler = false;
		DRS::GetSingleton()->targetScaleFactor = 1.0f;
		mMipLodBias = 0;
		return;
	}
	if (!mTempColor)
		mD3d11Device->CreateTexture2D(&desc, NULL, &mTempColor);
	mRenderSizeX = GetRenderWidth(0);
	mRenderSizeY = GetRenderHeight(0);
	mMotionScale[0] = mRenderSizeX;
	mMotionScale[1] = mRenderSizeY;
	SetMotionScaleX(0, mMotionScale[0]);
	SetMotionScaleY(0, mMotionScale[1]);
	mRenderScale = float(mRenderSizeX) / mDisplaySizeX;
	if (mEnableUpscaler) {
		DRS::GetSingleton()->targetScaleFactor = mRenderScale;
		DRS::GetSingleton()->ControlResolution();
		mMipLodBias = GetOptimalMipmapBias(0);
	} else {
		DRS::GetSingleton()->targetScaleFactor = 1.0f;
		DRS::GetSingleton()->ControlResolution();
		mMipLodBias = 0;
	}

}