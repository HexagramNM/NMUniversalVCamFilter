#include "stdafx.h"

NMVCamSource::NMVCamSource(IUnknown *pUnk, HRESULT *phr) :
	CSource(FILTER_NAME, pUnk, CLSID_NMUniversalVCam), 
	m_pin(NULL)
{
	m_pin = new NMVCamPin(phr, this);

	if(m_pin == NULL) {
		*phr=E_OUTOFMEMORY;
	}
}

NMVCamSource::~NMVCamSource() {
	delete m_pin;
}

CUnknown * WINAPI NMVCamSource::CreateInstance(IUnknown *pUnk, HRESULT *phr) {
	//�J�����ɐݒ肳���Ƃ��Ɉ�x�Ă΂�Ă���B
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


