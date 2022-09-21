#include "DRS.h"

#include <SimpleIni.h>

#include <ENB/AntTweakBar.h>
#include <ENB/ENBSeriesAPI.h>
extern ENB_API::ENBSDKALT1001* g_ENB;

#include <ReflexAPI.h>
extern ReflexAPI* g_Reflex;

#include "Nukem/GpuTimer.h"

#define GetSettingInt(a_section, a_setting, a_default) a_setting = (int)ini.GetLongValue(a_section, #a_setting, a_default);
#define SetSettingInt(a_section, a_setting) ini.SetLongValue(a_section, #a_setting, a_setting);

#define GetSettingFloat(a_section, a_setting, a_default) a_setting = (float)ini.GetDoubleValue(a_section, #a_setting, a_default);
#define SetSettingFloat(a_section, a_setting) ini.SetDoubleValue(a_section, #a_setting, a_setting);

#define GetSettingBool(a_section, a_setting, a_default) a_setting = ini.GetBoolValue(a_section, #a_setting, a_default);
#define SetSettingBool(a_section, a_setting) ini.SetBoolValue(a_section, #a_setting, a_setting);

void DRS::LoadINI()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(L"Data\\SKSE\\Plugins\\DynamicResolutionScaling.ini");
	GetSettingInt("Settings", iTargetFPS, 60);
	GetSettingFloat("Settings", fHighestScaleFactor, 1.0f);
	GetSettingFloat("Settings", fLowestScaleFactor, 0.7f);
	GetSettingBool("Settings", bEnableWithENB, false);
	ResetScale();
}

void DRS::GetGameSettings()
{
	auto ini = RE::INISettingCollection::GetSingleton();
	bEnableAutoDynamicResolution = ini->GetSetting("bEnableAutoDynamicResolution:Display");
	bEnableAutoDynamicResolution->data.b = true;
}

void DRS::Update()
{
	if (reset)
		ResetScale();
	else if (!(RE::UI::GetSingleton() && RE::UI::GetSingleton()->GameIsPaused()))  // Ignore paused game which skews frametimes
		ControlResolution();
}

void DRS::ControlResolution()
{
	float usage = GPUInfo::GetSingleton()->GetGPUUsage();
	if (usage <= 0.01f)  // When switching windows this can sometimes return 0.01 which creates delta spikes
		return;

	//assumedGPUtime = std::lerp(assumedGPUtime * usage, assumedGPUtime, lastCPUFrameTime / desiredFrameTime);

	float desiredFrameTime = 1000.0f / (float)iTargetFPS;
	float estGPUTime = g_GPUTimers.GetGPUTimeInMS(0);
	float unboundedGPUTime = estGPUTime * usage;
	if ((estGPUTime - desiredFrameTime) <= UnboundedHeadroomThreshold) {
		auto scale = (UnboundedHeadroomThreshold - (estGPUTime - desiredFrameTime)) * (1 / UnboundedHeadroomThreshold);
		estGPUTime = std::lerp(std::lerp(estGPUTime, unboundedGPUTime, (desiredFrameTime / estGPUTime) * scale), estGPUTime, lastCPUFrameTime / desiredFrameTime);
	} 
	logger::debug("Estimated GPU Time: {} Unbounded GPU Time: {}", estGPUTime, unboundedGPUTime);
	logger::debug("CPU frametime {} GPU usage {}", lastCPUFrameTime, usage);

	if (prevGPUFrameTime != 0) {
		float headroom = desiredFrameTime - estGPUTime;
		float GPUTimeDelta = estGPUTime - prevGPUFrameTime;
		logger::debug("Headroom: {} GPU Time Delta: {}", headroom, GPUTimeDelta);

		// If headroom is negative, we've exceeded target and need to scale down.
		if (headroom < 0.0) {
			scaleRaiseCounter = 0;

			// Since headroom is guaranteed to be negative here, we can add rather than negate and subtract.
			float scaleDecreaseFactor = std::lerp(headroom / desiredFrameTime, 0.0f, lastCPUFrameTime / desiredFrameTime);  // Accommodate for CPU spikes
			currentScaleFactor = std::clamp(currentScaleFactor + scaleDecreaseFactor, 0.0f, 1.0f);
		} else {
			// If delta is greater than headroom, we expect to exceed target and need to scale down.
			if (GPUTimeDelta > headroom) {
				scaleRaiseCounter = 0;
				float scaleDecreaseFactor = std::lerp(GPUTimeDelta / desiredFrameTime, 0.0f, lastCPUFrameTime / desiredFrameTime);  // Accommodate for CPU spikes
				currentScaleFactor = std::clamp(currentScaleFactor - scaleDecreaseFactor, 0.0f, 1.0f);
			} else {
				// If delta is negative, then perf is moving in a good direction and we can increment to scale up faster.
				if (GPUTimeDelta < 0.0) {
					scaleRaiseCounter += ScaleRaiseCounterBigIncrement * g_deltaTimeRealTime;
				} else {
					float headroomThreshold = estGPUTime * HeadroomThreshold;
					float deltaThreshold = estGPUTime * DeltaThreshold;

					// If we're too close to target or the delta is too large, do nothing out of concern that we could scale up and exceed target.
					// Otherwise, slow increment towards a scale up.
					if ((headroom > headroomThreshold) && (GPUTimeDelta < deltaThreshold)) {
						scaleRaiseCounter += ScaleRaiseCounterSmallIncrement * g_deltaTimeRealTime;
					}
				}

				if (scaleRaiseCounter >= ScaleRaiseCounterLimit) {
					scaleRaiseCounter = 0;

					// Headroom as percent of target is unlikely to use the full 0-1 range, so clamp on user settings and then remap to 0-1.
					float headroomPercent = headroom / desiredFrameTime;
					float clampedHeadroom = std::clamp(headroomPercent, ScaleHeadroomClampMin, ScaleHeadroomClampMax);
					float remappedHeadroom = (clampedHeadroom - ScaleHeadroomClampMin) / (ScaleHeadroomClampMax - ScaleHeadroomClampMin);
					float scaleIncreaseFactor = ScaleIncreaseBasis * std::lerp(ScaleIncreaseSmallFactor, ScaleIncreaseBigFactor, remappedHeadroom);
					currentScaleFactor = std::clamp(currentScaleFactor + scaleIncreaseFactor, 0.0f, 1.0f);
				}
			}
		}
		currentScaleFactor = std::clamp(currentScaleFactor, fLowestScaleFactor, fHighestScaleFactor);
		logger::debug("Current scale factor {}", currentScaleFactor);
	}
	prevGPUFrameTime = estGPUTime;
}

void DRS::ResetScale()
{
	currentScaleFactor = 1.0f;
	scaleRaiseCounter = 0;
}

bool GetENBParameterBool(const char* a_filename, const char* a_category, const char* a_keyname)
{
	BOOL                  bvalue;
	ENB_SDK::ENBParameter param;
	if (g_ENB->GetParameter(a_filename, a_category, a_keyname, &param)) {
		if (param.Type == ENB_SDK::ENBParameterType::ENBParam_BOOL) {
			memcpy(&bvalue, param.Data, ENBParameterTypeToSize(ENB_SDK::ENBParameterType::ENBParam_BOOL));
			return bvalue;
		}
	}
	logger::debug("Could not find ENB parameter {}:{}:{}", a_filename, a_category, a_keyname);
	return false;
}

void DRS::SetDRS(BSGraphics::State* a_state)
{
	if (bEnableAutoDynamicResolution && bEnableAutoDynamicResolution->GetBool() && (!(g_ENB && GetENBParameterBool("enbseries.ini", "GLOBAL", "UseEffect")) || bEnableWithENB)) {
		a_state->fDynamicResolutionPreviousHeightScale = a_state->fDynamicResolutionCurrentHeightScale;
		a_state->fDynamicResolutionPreviousWidthScale = a_state->fDynamicResolutionCurrentWidthScale;
		a_state->fDynamicResolutionCurrentHeightScale = currentScaleFactor;
		a_state->fDynamicResolutionCurrentWidthScale = currentScaleFactor;
	} else {
		a_state->fDynamicResolutionPreviousHeightScale = 1.0f;
		a_state->fDynamicResolutionPreviousWidthScale = 1.0f;
		a_state->fDynamicResolutionCurrentHeightScale = 1.0f;
		a_state->fDynamicResolutionCurrentWidthScale = 1.0f;
	}
}

void DRS::MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		GetGameSettings();
		LoadINI();
		break;

	case SKSE::MessagingInterface::kNewGame:
		LoadINI();
		break;

	case SKSE::MessagingInterface::kPreLoadGame:
		LoadINI();
		break;
	}
}

void DRS::UpdateCPUFrameTime()
{
	long long frameEnd = PerformanceCounter();
	double    elapsedMilliseconds = (double)((frameEnd - frameStart)) / PerformanceFrequency();
	float     frameTime = (float)elapsedMilliseconds;
	lastCPUFrameTime = frameTime;
}

// Fader Menu
// Mist Menu
// Loading Menu
// LoadWaitSpinner

RE::BSEventNotifyControl MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event->menuName == "Loading Menu") {
		if (a_event->opening) {
			DRS::GetSingleton()->reset = true;
		} else {
			DRS::GetSingleton()->reset = false;
		}
	}

	return RE::BSEventNotifyControl::kContinue;
}

bool MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto                             ui = RE::UI::GetSingleton();

	if (!ui) {
		logger::error("UI event source not found");
		return false;
	}

	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

	logger::info("Registered {}", typeid(singleton).name());

	return true;
}
