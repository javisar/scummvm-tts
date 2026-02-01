/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// Disable symbol overrides so that we can use system headers.
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#if defined(USE_TTS) && defined(WIN32)
#include <basetyps.h>
#include <windows.h>
#include <servprov.h>

#include <sapi.h>
#include <sphelper.h>
#include <atlbase.h>
#if _SAPI_VER < 0x53
#define SPF_PARSE_SAPI 0x80
#endif

// WinHTTP for async dialogue capture
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include "backends/platform/sdl/win32/win32_wrapper.h"

#include "backends/text-to-speech/windows/windows-text-to-speech.h"


#include "common/translation.h"
#include "common/system.h"
#include "common/ustr.h"
#include "common/config-manager.h"
#include <vector>

ISpVoice *_voice;

// We need this pointer to be able to stop speech immediately.
ISpAudio *_audio;

WindowsTextToSpeechManager::WindowsTextToSpeechManager()
	: _speechState(BROKEN){
	init();
	_threadParams.queue = &_speechQueue;
	_threadParams.state = &_speechState;
	_threadParams.mutex = &_speechMutex;
	_thread = nullptr;
	_speechMutex = CreateMutex(nullptr, FALSE, nullptr);
	if (_speechMutex == nullptr) {
		_speechState = BROKEN;
		warning("Could not create TTS mutex");
	}
}

void WindowsTextToSpeechManager::init() {
	// init COM
	if (FAILED(::CoInitialize(nullptr)))
		return;

	// init audio
	ISpObjectTokenCategory *pTokenCategory;
	HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, (void **)&pTokenCategory);
	if (SUCCEEDED(hr)) {
		hr = pTokenCategory->SetId(SPCAT_AUDIOOUT, TRUE);
		if (SUCCEEDED(hr)) {
			WCHAR *tokenId;
			hr = pTokenCategory->GetDefaultTokenId(&tokenId);
			if (SUCCEEDED(hr)) {
				ISpObjectToken *pToken;
				hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_ISpObjectToken, (void **)&pToken);
				if (SUCCEEDED(hr)) {
					hr = pToken->SetId(nullptr, tokenId, FALSE);
					if (SUCCEEDED(hr)) {
						hr = pToken->CreateInstance(nullptr, CLSCTX_ALL, IID_ISpAudio, (void **)&_audio);
					}
				}
				CoTaskMemFree(tokenId);
			}
		}
	}
	if (FAILED(hr)) {
		warning("Could not initialize TTS audio");
		return;
	}

	// init voice
	hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, (void **)&_voice);
	if (FAILED(hr)) {
		warning("Could not initialize TTS voice");
		return;
	}

	_speechState = NO_VOICE;

#ifdef USE_TRANSLATION
	setLanguage(TransMan.getCurrentLanguage());
#else
	setLanguage("en");
#endif

	_voice->SetOutput(_audio, FALSE);

	if (!_ttsState->_availableVoices.empty())
		_speechState = READY;
	else
		_speechState = NO_VOICE;
	_lastSaid = "";
	while (!_speechQueue.empty()) {
		free(_speechQueue.front());
		_speechQueue.pop_front();
	}
}

WindowsTextToSpeechManager::~WindowsTextToSpeechManager() {
	stop();

	clearState();

	if (_thread != nullptr) {
		WaitForSingleObject(_thread, INFINITE);
		CloseHandle(_thread);
	}
	if (_speechMutex != nullptr) {
		CloseHandle(_speechMutex);
	}
	if (_voice)
		_voice->Release();
	::CoUninitialize();
}

DWORD WINAPI startSpeech(LPVOID parameters) {
	WindowsTextToSpeechManager::SpeechParameters *params =
		(WindowsTextToSpeechManager::SpeechParameters *) parameters;
	// wait for the previous speech, if the previous thread exited too early
	_voice->WaitUntilDone(INFINITE);

	while (!params->queue->empty()) {
		WaitForSingleObject(*params->mutex, INFINITE);
		// check again, when we have exclusive access to the queue
		if (params->queue->empty() || *(params->state) == WindowsTextToSpeechManager::PAUSED) {
			ReleaseMutex(*params->mutex);
			break;
		}
		WCHAR *currentSpeech = params->queue->front();
		_voice->Speak(currentSpeech, SPF_PURGEBEFORESPEAK | SPF_ASYNC | SPF_PARSE_SAPI, nullptr);
		ReleaseMutex(*params->mutex);

		while (*(params->state) != WindowsTextToSpeechManager::PAUSED)
			if (_voice->WaitUntilDone(10) == S_OK)
				break;

		WaitForSingleObject(*params->mutex, INFINITE);
		if (!params->queue->empty() && params->queue->front() == currentSpeech) {
			if (currentSpeech != nullptr)
				free(currentSpeech);
			params->queue->pop_front();
		}
		ReleaseMutex(*params->mutex);
	}

	WaitForSingleObject(*params->mutex, INFINITE);
	if (*(params->state) != WindowsTextToSpeechManager::PAUSED)
		*(params->state) = WindowsTextToSpeechManager::READY;
	ReleaseMutex(*params->mutex);
	return 0;
}

bool WindowsTextToSpeechManager::say(const Common::U32String &str, Action action) {
	if (_speechState == BROKEN || _speechState == NO_VOICE) {
		if (_ttsState->_enabled)
			warning("The text to speech cannot speak in this state");
		return true;
	}

	if (isSpeaking() && action == DROP)
		return true;

	// We have to set the pitch by prepending xml code at the start of the said string;
	Common::U32String pitch = Common::U32String::format("<pitch absmiddle=\"%d\"/>%S", _ttsState->_pitch / 10, str.c_str());
	WCHAR *strW = (WCHAR *) pitch.encodeUTF16Native();
	if (strW == nullptr) {
		warning("Cannot convert from UTF-32 encoding for text to speech");
		return true;
	}

	WaitForSingleObject(_speechMutex, INFINITE);
	if (isSpeaking() && !_speechQueue.empty() && action == INTERRUPT_NO_REPEAT &&
			_speechQueue.front() != NULL && !wcscmp(_speechQueue.front(), strW)) {
		while (_speechQueue.size() != 1) {
			free(_speechQueue.back());
			_speechQueue.pop_back();
		}
		free(strW);
		ReleaseMutex(_speechMutex);
		return true;
	}

	if (isSpeaking() && !_speechQueue.empty() && action == QUEUE_NO_REPEAT &&
			_speechQueue.front() != NULL &&!wcscmp(_speechQueue.back(), strW)) {
		ReleaseMutex(_speechMutex);
		return true;
	}

	ReleaseMutex(_speechMutex);
	if ((isPaused() || isSpeaking()) && (action == INTERRUPT || action == INTERRUPT_NO_REPEAT)) {
		stop();
	}

	WaitForSingleObject(_speechMutex, INFINITE);
	_speechQueue.push_back(strW);
	ReleaseMutex(_speechMutex);

	if (!isSpeaking() && !isPaused()) {
		DWORD threadId;
		if (_thread != nullptr) {
			WaitForSingleObject(_thread, INFINITE);
			CloseHandle(_thread);
		}
		_speechState = SPEAKING;
		_thread = CreateThread(nullptr, 0, startSpeech, &_threadParams, 0, &threadId);
		if (_thread == nullptr) {
			warning("Could not create speech thread");
			_speechState = READY;
			return true;
		}
	}
	return false;
}

bool WindowsTextToSpeechManager::sayExtended(const Common::U32String &str, Action action, uint32 hash, byte actor, int room) {
	// Async HTTP capture (non-blocking, fire and forget)
	captureDialogueAsync(str, actor, room);

	if (_speechState == BROKEN || _speechState == NO_VOICE) {
		if (_ttsState->_enabled)
			warning("The text to speech cannot speak in this state");
		return true;
	}

	if (isSpeaking() && action == DROP)
		return true;

	// We have to set the pitch by prepending xml code at the start of the said string;
	//Common::U32String pitch = Common::U32String::format("<pitch absmiddle=\"%d\"/>%S", _ttsState->_pitch / 10, str.c_str());
	Common::U32String pitch = Common::U32String::format("<pitch absmiddle=\"%d\"/>%S", _ttsState->_pitch / 10, str.c_str());
	pitch = Common::U32String::format("<context hash=\"%i\" actor=\"%i\" room=\"%i\"/>%S", hash, actor, room, pitch.c_str() );
	WCHAR *strW = (WCHAR *) pitch.encodeUTF16Native();
	if (strW == nullptr) {
		warning("Cannot convert from UTF-32 encoding for text to speech");
		return true;
	}

	WaitForSingleObject(_speechMutex, INFINITE);
	if (isSpeaking() && !_speechQueue.empty() && action == INTERRUPT_NO_REPEAT &&
			_speechQueue.front() != NULL && !wcscmp(_speechQueue.front(), strW)) {
		while (_speechQueue.size() != 1) {
			free(_speechQueue.back());
			_speechQueue.pop_back();
		}
		free(strW);
		ReleaseMutex(_speechMutex);
		return true;
	}

	if (isSpeaking() && !_speechQueue.empty() && action == QUEUE_NO_REPEAT &&
			_speechQueue.front() != NULL &&!wcscmp(_speechQueue.back(), strW)) {
		ReleaseMutex(_speechMutex);
		return true;
	}

	ReleaseMutex(_speechMutex);
	if ((isPaused() || isSpeaking()) && (action == INTERRUPT || action == INTERRUPT_NO_REPEAT)) {
		stop();
	}

	WaitForSingleObject(_speechMutex, INFINITE);
	_speechQueue.push_back(strW);
	ReleaseMutex(_speechMutex);

	if (!isSpeaking() && !isPaused()) {
		DWORD threadId;
		if (_thread != nullptr) {
			WaitForSingleObject(_thread, INFINITE);
			CloseHandle(_thread);
		}
		_speechState = SPEAKING;
		_thread = CreateThread(nullptr, 0, startSpeech, &_threadParams, 0, &threadId);
		if (_thread == nullptr) {
			warning("Could not create speech thread");
			_speechState = READY;
			return true;
		}
	}
	return false;
}

bool WindowsTextToSpeechManager::stop() {
	if (_speechState == BROKEN || _speechState == NO_VOICE)
		return true;
	if (isPaused())
		resume();
	_audio->SetState(SPAS_STOP, 0);
	WaitForSingleObject(_speechMutex, INFINITE);
	// Delete the speech queue
	while (!_speechQueue.empty()) {
		if (_speechQueue.front() != NULL)
			free(_speechQueue.front());
		_speechQueue.pop_front();
	}
	// Stop the current speech
	_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK | SPF_ASYNC, nullptr);
	_speechState = READY;
	ReleaseMutex(_speechMutex);
	_audio->SetState(SPAS_RUN, 0);
	return false;
}

bool WindowsTextToSpeechManager::pause() {
	if (_speechState == BROKEN || _speechState == NO_VOICE)
		return true;
	if (isPaused())
		return false;
	WaitForSingleObject(_speechMutex, INFINITE);
	_voice->Pause();
	_speechState = PAUSED;
	ReleaseMutex(_speechMutex);
	return false;
}

bool WindowsTextToSpeechManager::resume() {
	if (_speechState == BROKEN || _speechState == NO_VOICE)
		return true;
	if (!isPaused())
		return false;
	_voice->Resume();
	DWORD threadId;
	if (_thread != nullptr) {
		WaitForSingleObject(_thread, INFINITE);
		CloseHandle(_thread);
	}
	_speechState = SPEAKING;
	_thread = CreateThread(nullptr, 0, startSpeech, &_threadParams, 0, &threadId);
	if (_thread == nullptr) {
		warning("Could not create speech thread");
		_speechState = READY;
		return true;
	}
	return false;
}

bool WindowsTextToSpeechManager::isSpeaking() {
	return _speechState == SPEAKING;
}

bool WindowsTextToSpeechManager::isPaused() {
	return _speechState == PAUSED;
}

bool WindowsTextToSpeechManager::isReady() {
	if (_speechState == BROKEN || _speechState == NO_VOICE)
		return false;
	if (_speechState != PAUSED && !isSpeaking())
		return true;
	else
		return false;
}

void WindowsTextToSpeechManager::setVoice(unsigned index) {
	if (_speechState == BROKEN || _speechState == NO_VOICE)
		return;
	_voice->SetVoice((ISpObjectToken *) _ttsState->_availableVoices[index].getData());
	_ttsState->_activeVoice = index;
}

void WindowsTextToSpeechManager::setRate(int rate) {
	if (_speechState == BROKEN || _speechState == NO_VOICE)
		return;
	assert(rate >= -100 && rate <= 100);
	_voice->SetRate(rate / 10);
	_ttsState->_rate = rate;
}

void WindowsTextToSpeechManager::setPitch(int pitch) {
	if (_speechState == BROKEN || _speechState == NO_VOICE)
		return;
	assert(pitch >= -100 && pitch <= 100);
	_ttsState->_pitch = pitch;
}

void WindowsTextToSpeechManager::setVolume(unsigned volume) {
	if (_speechState == BROKEN || _speechState == NO_VOICE)
		return;
	assert(volume <= 100);
	_voice->SetVolume(volume);
	_ttsState->_volume = volume;
}

void WindowsTextToSpeechManager::setLanguage(Common::String language) {
	if (_ttsState->_language != language.substr(0, 2) || _ttsState->_availableVoices.empty()) {
		Common::TextToSpeechManager::setLanguage(language);
		updateVoices();
	} else if (_speechState == NO_VOICE) {
		_speechState = READY;
	}
	setVoice(0);
}

static void dumpTokenInfo(ISpObjectToken *token) {
	if (!token)
		return;
	// Id del token
	LPWSTR id = nullptr;
	if (SUCCEEDED(token->GetId(&id))) {
		char *idA = Win32::unicodeToAnsi(id);
		warning("TTS token Id: %s", idA);
		free(idA);
		CoTaskMemFree(id);
	}

	// Descripci�n (GetStringValue(nullptr))
	WCHAR *descW = nullptr;
	if (SUCCEEDED(token->GetStringValue(nullptr, &descW))) {
		char *descA = Win32::unicodeToAnsi(descW);
		warning("TTS token Desc: %s", descA);
		free(descA);
		CoTaskMemFree(descW);
	}

	// Atributos: Language, Gender, Age, Name, Vendor...
	ISpDataKey *key = nullptr;
	if (SUCCEEDED(token->OpenKey(L"Attributes", &key))) {
		LPWSTR val = nullptr;
		const wchar_t *attrs[] = {L"Language", L"Gender", L"Age", L"Name", L"Vendor", nullptr};
		for (const wchar_t **p = attrs; *p; ++p) {
			if (SUCCEEDED(key->GetStringValue(*p, &val))) {
				char *vA = Win32::unicodeToAnsi(val);
				warning("TTS token Attr %S: %s", *p, vA);
				free(vA);
				CoTaskMemFree(val);
			}
		}
		key->Release();
	}
}

void WindowsTextToSpeechManager::createVoice(void *cpVoiceToken) {
	ISpObjectToken *voiceToken = (ISpObjectToken *) cpVoiceToken;

	// description
	WCHAR *descW;
	char *buffer;
	Common::String desc;
	HRESULT hr = voiceToken->GetStringValue(nullptr, &descW);
	if (SUCCEEDED(hr)) {
		buffer = Win32::unicodeToAnsi(descW);
		desc = buffer;
		free(buffer);
		CoTaskMemFree(descW);
	}

	if (desc == "Sample TTS Voice") {
		// This is a really bad voice, it is basically unusable
		return;
	}

	// voice attributes
	ISpDataKey *key = nullptr;
	hr = voiceToken->OpenKey(L"Attributes", &key);

	if (FAILED(hr)) {
		voiceToken->Release();
		warning("Could not open attribute key for voice: %s", desc.c_str());
		return;
	}
	LPWSTR data;

	// language
	hr = key->GetStringValue(L"Language", &data);
	if (FAILED(hr)) {
		voiceToken->Release();
		warning("Could not get the language attribute for voice: %s", desc.c_str());
		return;
	}
	Common::String language = lcidToLocale(wcstol(data, nullptr, 16));
	CoTaskMemFree(data);

	// only get the voices for the current language
	if (language != _ttsState->_language) {
		voiceToken->Release();
		return;
	}

	// gender
	hr = key->GetStringValue(L"Gender", &data);
	if (FAILED(hr)) {
		voiceToken->Release();
		warning("Could not get the gender attribute for voice: %s", desc.c_str());
		return;
	}
	Common::TTSVoice::Gender gender = !wcscmp(data, L"Male") ? Common::TTSVoice::MALE : Common::TTSVoice::FEMALE;
	CoTaskMemFree(data);

	// age
	hr = key->GetStringValue(L"Age", &data);
	if (FAILED(hr)) {
		voiceToken->Release();
		warning("Could not get the age attribute for voice: %s", desc.c_str());
		return;
	}
	Common::TTSVoice::Age age = !wcscmp(data, L"Adult") ? Common::TTSVoice::ADULT : Common::TTSVoice::UNKNOWN_AGE;
	CoTaskMemFree(data);

	_ttsState->_availableVoices.push_back(Common::TTSVoice(gender, age, (void *) voiceToken, desc));
}

Common::String WindowsTextToSpeechManager::lcidToLocale(LCID locale) {
	int nchars = GetLocaleInfo(locale, LOCALE_SISO639LANGNAME, nullptr, 0);
	TCHAR *languageCode = new TCHAR[nchars];
	GetLocaleInfo(locale, LOCALE_SISO639LANGNAME, languageCode, nchars);
	Common::String result = Win32::tcharToString(languageCode);
	delete[] languageCode;
	return result;
}

void WindowsTextToSpeechManager::updateVoices() {
	if (!_ttsState->_enabled) {
		_speechState = NO_VOICE;
		return;
	}

	if (_speechState == BROKEN)
		return;

	_ttsState->_availableVoices.clear();
	ISpObjectToken *cpVoiceToken = nullptr;
	IEnumSpObjectTokens *cpEnum = nullptr;
	unsigned long ulCount = 0;

	ISpObjectTokenCategory *cpCategory;
	HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, (void**)&cpCategory);
	if (SUCCEEDED(hr)) {
		hr = cpCategory->SetId(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices", FALSE);
		if (!SUCCEEDED(hr)) {
			hr = cpCategory->SetId(SPCAT_VOICES, FALSE);
		}

		if (SUCCEEDED(hr)) {
			hr = cpCategory->EnumTokens(nullptr, nullptr, &cpEnum);
		}
	}

	if (SUCCEEDED(hr)) {
		hr = cpEnum->GetCount(&ulCount);
	}
	_voice->SetVolume(0);
	while (SUCCEEDED(hr) && ulCount--) {
		hr = cpEnum->Next(1, &cpVoiceToken, nullptr);
		dumpTokenInfo(cpVoiceToken);
		_voice->SetVoice(cpVoiceToken);
		if (SUCCEEDED(_voice->Speak(L"hi, this is test", SPF_PURGEBEFORESPEAK | SPF_ASYNC | SPF_IS_NOT_XML, nullptr)))
			createVoice(cpVoiceToken);
		else
			cpVoiceToken->Release();
	}
	// stop the test speech, we don't use stop(), because we don't wan't it to set state to READY
	// and we could easily be in NO_VOICE or BROKEN state here, in which the stop() wouldn't work
	_audio->SetState(SPAS_STOP, 0);
	_audio->SetState(SPAS_RUN, 0);
	_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK | SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
	_voice->SetVolume(_ttsState->_volume);
	cpEnum->Release();

	if (_ttsState->_availableVoices.empty()) {
		_speechState = NO_VOICE;
		warning("No voice is available for language: %s", _ttsState->_language.c_str());
	} else if (_speechState == NO_VOICE)
		_speechState = READY;
}

void WindowsTextToSpeechManager::freeVoiceData(void *data) {
	ISpObjectToken *voiceToken = (ISpObjectToken *) data;
	voiceToken->Release();
}

// Async dialogue capture implementation
void WindowsTextToSpeechManager::captureDialogueAsync(const Common::U32String &text, byte actor, int room) {
	// Create copy of data for thread (will be freed by thread)
	CaptureParams* params = new CaptureParams;
	params->text = text.encode(Common::kUtf8);
	params->actor = actor;
	params->room = room;
	// TODO: Make gameid configurable - hardcoded for Phase 1 (Indy 3)
	params->gameid = "indy3";

	// Fire and forget - don't wait for result
	HANDLE thread = CreateThread(nullptr, 0, captureDialogueThread, params, 0, nullptr);
	if (thread) {
		CloseHandle(thread);  // Don't need handle, thread runs independently
	} else {
		// Thread creation failed - clean up params
		delete params;
		warning("Failed to create dialogue capture thread");
	}
}

DWORD WINAPI WindowsTextToSpeechManager::captureDialogueThread(LPVOID param) {
	CaptureParams* params = (CaptureParams*)param;

	// Build JSON payload
	// Escape quotes and backslashes in text
	Common::String escapedText = params->text;
	Common::String temp;
	for (uint i = 0; i < escapedText.size(); i++) {
		char c = escapedText[i];
		if (c == '"' || c == '\\') {
			temp += '\\';
		}
		temp += c;
	}
	escapedText = temp;

	Common::String jsonPayload = Common::String::format(
		"{\"text\":\"%s\",\"actor_id\":%d,\"room_id\":%d,\"gameid\":\"%s\",\"engine\":\"scumm\"}",
		escapedText.c_str(),
		params->actor,
		params->room,
		params->gameid.c_str()
	);

	// Convert JSON to wide string for WinHTTP
	int jsonLen = jsonPayload.size();
	DWORD dataLen = jsonLen;

	// Initialize WinHTTP
	HINTERNET hSession = WinHttpOpen(
		L"ScummVM/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,
		0
	);

	if (!hSession) {
		OutputDebugStringA("Dialogue capture: Failed to open WinHTTP session\n");
		delete params;
		return 1;
	}

	// Connect to localhost:8880
	HINTERNET hConnect = WinHttpConnect(
		hSession,
		L"localhost",
		8880,
		0
	);

	if (!hConnect) {
		OutputDebugStringA("Dialogue capture: Failed to connect to localhost:8880\n");
		WinHttpCloseHandle(hSession);
		delete params;
		return 1;
	}

	// Open request
	HINTERNET hRequest = WinHttpOpenRequest(
		hConnect,
		L"POST",
		L"/capture/register",
		nullptr,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		0
	);

	if (!hRequest) {
		OutputDebugStringA("Dialogue capture: Failed to open HTTP request\n");
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		delete params;
		return 1;
	}

	// Set headers
	LPCWSTR headers = L"Content-Type: application/json\r\n";
	BOOL headersResult = WinHttpAddRequestHeaders(
		hRequest,
		headers,
		(DWORD)-1L,
		WINHTTP_ADDREQ_FLAG_ADD
	);

	// Send request
	BOOL sendResult = WinHttpSendRequest(
		hRequest,
		WINHTTP_NO_ADDITIONAL_HEADERS,
		0,
		(LPVOID)jsonPayload.c_str(),
		dataLen,
		dataLen,
		0
	);

	if (!sendResult) {
		// Silently fail - backend might be down, game should continue
		OutputDebugStringA("Dialogue capture: Failed to send HTTP request (backend may be offline)\n");
	} else {
		// Receive response (but don't block on it)
		BOOL receiveResult = WinHttpReceiveResponse(hRequest, nullptr);
		if (receiveResult) {
			DWORD statusCode = 0;
			DWORD statusCodeSize = sizeof(statusCode);
			WinHttpQueryHeaders(
				hRequest,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX,
				&statusCode,
				&statusCodeSize,
				WINHTTP_NO_HEADER_INDEX
			);

			if (statusCode == 200) {
				// Success - log to debug output
				OutputDebugStringA("Dialogue capture: Successfully captured dialogue\n");
			} else {
				char errorMsg[256];
				sprintf(errorMsg, "Dialogue capture: HTTP %lu error\n", statusCode);
				OutputDebugStringA(errorMsg);
			}
		}
	}

	// Clean up
	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	delete params;

	return 0;
}

#endif
