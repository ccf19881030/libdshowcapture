/*
 *  Copyright (C) 2014 Hugh Bailey <obs.jim@gmail.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 */

#include "dshow-base.hpp"
#include "dshow-enum.hpp"
#include "log.hpp"

#include <bdaiface.h>

#include <vector>
#include <string>

using namespace std;

namespace DShow {

struct DeviceFilterCallbackInfo {
	CComPtr<IBaseFilter> filter;
	const wchar_t        *name;
	const wchar_t        *path;
};

static bool GetDeviceCallback(DeviceFilterCallbackInfo &info,
		IBaseFilter *filter, const wchar_t *name, const wchar_t *path)
{
	if (info.name && wcscmp(name, info.name) != 0)
		return true;

	info.filter = filter;

	/* continue if path does not match */
	if (!path || !info.path || wcscmp(path, info.path) != 0)
		return true;

	return false;
}

bool GetDeviceFilter(const IID &type, const wchar_t *name, const wchar_t *path,
		IBaseFilter **out)
{
	DeviceFilterCallbackInfo info;
	info.name = name;
	info.path = path;

	if (!EnumDevices(type, EnumDeviceCallback(GetDeviceCallback), &info))
		return false;

	if (info.filter != NULL) {
		*out = info.filter.Detach();
		return true;
	}

	return false;
}

/* checks to see if a pin's config caps have a specific media type */
static bool PinConfigHasMajorType(IPin *pin, const GUID &type)
{
	HRESULT hr;
	CComPtr<IAMStreamConfig> config;
	int count, size;

	hr = pin->QueryInterface(IID_IAMStreamConfig, (void**)&config);
	if (FAILED(hr))
		return false;

	hr = config->GetNumberOfCapabilities(&count, &size);
	if (FAILED(hr))
		return false;

	vector<BYTE> caps;
	caps.resize(size);

	for (int i = 0; i < count; i++) {
		MediaTypePtr mt;
		if (SUCCEEDED(config->GetStreamCaps(i, &mt, caps.data())))
			if (mt->majortype == type)
				return true;
	}

	return false;
}

/* checks to see if a pin has a certain major media type */
static bool PinHasMajorType(IPin *pin, const GUID &type)
{
	HRESULT hr;
	MediaTypePtr mt;
	CComPtr<IEnumMediaTypes> mediaEnum;

	/* first, check the config caps. */
	if (PinConfigHasMajorType(pin, type))
		return true;

	/* then let's check the media type for the pin */
	if (FAILED(pin->EnumMediaTypes(&mediaEnum)))
		return false;

	ULONG curVal;
	hr = mediaEnum->Next(1, &mt, &curVal);
	if (hr != S_OK)
		return false;

	return mt->majortype == type;
}

static inline bool PinIsDirection(IPin *pin, PIN_DIRECTION dir)
{
	if (!pin)
		return false;

	PIN_DIRECTION pinDir;
	return SUCCEEDED(pin->QueryDirection(&pinDir)) && pinDir == dir;
}

static HRESULT GetPinCategory(IPin *pin, GUID &category)
{
	if (!pin)
		return E_POINTER;

	CComQIPtr<IKsPropertySet> propertySet(pin);
	DWORD                     size;

	if (propertySet == NULL)
		return E_NOINTERFACE;

	return propertySet->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY,
			NULL, 0, &category, sizeof(GUID), &size);
}

static inline bool PinIsCategory(IPin *pin, const GUID &category)
{
	if (!pin) return false;

	GUID pinCategory;
	HRESULT hr = GetPinCategory(pin, pinCategory);

	/* if the pin has no category interface, chances are we created it */
	if (FAILED(hr))
		return (hr == E_NOINTERFACE);

	return category == pinCategory;
}

static inline bool PinNameIs(IPin *pin, const wchar_t *name)
{
	if (!pin) return false;
	if (!name) return true;

	PIN_INFO pinInfo;

	if (FAILED(pin->QueryPinInfo(&pinInfo)))
		return false;

	if (pinInfo.pFilter)
		pinInfo.pFilter->Release();

	return wcscmp(name, pinInfo.achName) == 0;
}

static inline bool PinMatches(IPin *pin, const GUID &type, const GUID &category,
		PIN_DIRECTION &dir)
{
	if (!PinHasMajorType(pin, type))
		return false;
	if (!PinIsDirection(pin, dir))
		return false;
	if (!PinIsCategory(pin, category))
		return false;

	return true;
}

bool GetFilterPin(IBaseFilter *filter, const GUID &type, const GUID &category,
		PIN_DIRECTION dir, IPin **pin)
{
	CComPtr<IPin>      curPin;
	CComPtr<IEnumPins> pinsEnum;
	ULONG              num;

	if (!filter)
		return false;
	if (FAILED(filter->EnumPins(&pinsEnum)))
		return false;

	while (pinsEnum->Next(1, &curPin, &num) == S_OK) {

		if (PinMatches(curPin, type, category, dir)) {
			*pin = curPin;
			(*pin)->AddRef();
			return true;
		}

		curPin.Release();
	}

	return false;
}

bool GetPinByName(IBaseFilter *filter, PIN_DIRECTION dir, const wchar_t *name,
		IPin **pin)
{
	CComPtr<IPin>      curPin;
	CComPtr<IEnumPins> pinsEnum;
	ULONG              num;

	if (!filter)
		return false;
	if (FAILED(filter->EnumPins(&pinsEnum)))
		return false;

	while (pinsEnum->Next(1, &curPin, &num) == S_OK) {
		wstring pinName;

		if (PinIsDirection(curPin, dir) && PinNameIs(curPin, name)) {
			*pin = curPin.Detach();
			return true;
		}

		curPin.Release();
	}

	return false;
}

bool GetPinByMedium(IBaseFilter *filter, REGPINMEDIUM &medium, IPin **pin)
{
	CComPtr<IPin>      curPin;
	CComPtr<IEnumPins> pinsEnum;
	ULONG              num;

	if (!filter)
		return false;
	if (FAILED(filter->EnumPins(&pinsEnum)))
		return false;

	while (pinsEnum->Next(1, &curPin, &num) == S_OK) {
		REGPINMEDIUM curMedium;

		if (GetPinMedium(curPin, curMedium) &&
		    memcmp(&medium, &curMedium, sizeof(medium)) == 0) {
			*pin = curPin.Detach();
			return true;
		}

		curPin.Release();
	}

	return false;
}

static bool GetFilterByMediumFromMoniker(IMoniker *moniker,
		REGPINMEDIUM &medium, IBaseFilter **filter)
{
	CComPtr<IBaseFilter> curFilter;
	HRESULT              hr;

	hr = moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter,
			(void**)&curFilter);
	if (SUCCEEDED(hr)) {
		CComPtr<IPin> pin;
		if (GetPinByMedium(curFilter, medium, &pin)) {
			*filter = curFilter.Detach();
			return true;
		}
	} else {
		WarningHR(L"GetFilterByMediumFromMoniker: BindToObject failed",
				hr);
	}

	return false;
}

bool GetFilterByMedium(const CLSID &id, REGPINMEDIUM &medium,
		IBaseFilter **filter)
{
	CComPtr<ICreateDevEnum> deviceEnum;
	CComPtr<IEnumMoniker>   enumMoniker;
	CComPtr<IMoniker>       moniker;
	DWORD                   count = 0;
	HRESULT                 hr;

	hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
			CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
			(void**)&deviceEnum);
	if (FAILED(hr)) {
		WarningHR(L"GetFilterByMedium: Failed to create device enum",
				hr);
		return false;
	}

	hr = deviceEnum->CreateClassEnumerator(id, &enumMoniker, 0);
	if (FAILED(hr)) {
		WarningHR(L"GetFilterByMedium: Failed to create enum moniker",
				hr);
		return false;
	}

	enumMoniker->Reset();

	while (enumMoniker->Next(1, &moniker, &count) == S_OK) {
		if (GetFilterByMediumFromMoniker(moniker, medium, filter))
			return true;

		moniker.Release();
	}

	return false;
}

bool GetPinMedium(IPin *pin, REGPINMEDIUM &medium)
{
	CComQIPtr<IKsPin>             ksPin(pin);
	CoTaskMemPtr<KSMULTIPLE_ITEM> items;

	if (!ksPin)
		return false;

	if (FAILED(ksPin->KsQueryMediums(&items)))
		return false;

	REGPINMEDIUM *curMed = reinterpret_cast<REGPINMEDIUM*>(items + 1);
	for (ULONG i = 0; i < items->Count; i++, curMed++) {
		if (!IsEqualGUID(curMed->clsMedium, GUID_NULL) &&
		    !IsEqualGUID(curMed->clsMedium, KSMEDIUMSETID_Standard)) {
			medium = *curMed;
			return true;
		}
	}

	memset(&medium, 0, sizeof(medium));
	return false;
}

wstring ConvertHRToEnglish(HRESULT hr)
{
	LPWSTR buffer = NULL;
	wstring str;

	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, hr, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
			(LPTSTR)&buffer, 0, NULL);

	if (buffer) {
		str = buffer;
		LocalFree(buffer);
	}

	return str.c_str();
}

}; /* namespace DShow */
