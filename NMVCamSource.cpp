#include "stdafx.h"

NMVCamSource::NMVCamSource(IUnknown *pUnk, HRESULT *phr) :
	CSource(FILTER_NAME, pUnk, CLSID_NMUniversalVCam), 
	_pin(NULL)
{
	_pin = new NMVCamPin(phr, this);

	if(_pin == NULL) {
		*phr=E_OUTOFMEMORY;
	}
}

NMVCamSource::~NMVCamSource() {
	delete _pin;
}

CUnknown * WINAPI NMVCamSource::CreateInstance(IUnknown *pUnk, HRESULT *phr) {
	//ƒJƒƒ‰‚ÉÝ’è‚³‚ê‚é‚Æ‚«‚Éˆê“xŒÄ‚Î‚ê‚Ä‚¢‚éB
	NMVCamSource *pNewFilter = new NMVCamSource(pUnk, phr);
	if (pNewFilter == NULL) {
		*phr = E_OUTOFMEMORY;
	}
	return pNewFilter;
}

STDMETHODIMP NMVCamSource::QueryInterface(REFIID riid, void **ppv) {
	if (riid == _uuidof(IAMStreamConfig) || riid == _uuidof(IKsPropertySet)) {
		return m_paStreams[0]->QueryInterface(riid, ppv);
	}
	else {
		return CSource::QueryInterface(riid, ppv);
	}
}


