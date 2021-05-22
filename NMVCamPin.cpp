#include "stdafx.h"

using namespace winrt;
using namespace winrt::Windows::System;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Graphics::Capture;

NMVCamPin::NMVCamPin(HRESULT *phr, NMVCamSource *pFilter) : CSourceStream(NAME("NMVCamPin"), phr, pFilter, OUTPUT_PIN_NAME)
	,m_pFilter(pFilter)
	,m_isSelectingWindow(false), m_pickerActivate(false), m_reverseOutput(false), m_previousChangeReverseOutput(false)
	,m_deviceCtx(nullptr), m_bufferTexture(nullptr), m_attatchedWindow(NULL)
	,m_graphicsCaptureItem(nullptr), m_framePool(nullptr)
	,m_captureSession(nullptr), m_frameBits(nullptr), m_pixelPosX(nullptr), m_pixelPosY(nullptr)
	,m_lpDI(NULL), m_lpKeyboard(NULL), m_capturePickerThread(nullptr)
	,m_pickerResult(S_OK), m_rtFrameLength(666666)
	,m_BmpData(NULL), m_Hdc(NULL), m_Bitmap(NULL)
{
	GetMediaType(&m_mt);
	m_brush = CreateSolidBrush(RGB(0, 0, 0));

	//CPU読み出し可能なバッファをGPU上に作成
	m_capWinSize.Width = 1;
	m_capWinSize.Height = 1;
	m_capWinSizeInTexture.left = 0;
	m_capWinSizeInTexture.right = 1;
	m_capWinSizeInTexture.top = 0;
	m_capWinSizeInTexture.bottom = 1;
	m_capWinSizeInTexture.front = 0;
	m_capWinSizeInTexture.back = 1;
	m_frameBits = new unsigned char[WINDOW_WIDTH * WINDOW_HEIGHT * ((PIXEL_BIT - 1) / 8 + 1)];
	m_pixelPosX = new double[WINDOW_WIDTH * WINDOW_HEIGHT];
	m_pixelPosY = new double[WINDOW_WIDTH * WINDOW_HEIGHT];
	for (int idx = 0; idx < WINDOW_WIDTH * WINDOW_HEIGHT; idx++) {
		m_pixelPosX[idx] = -1;
		m_pixelPosY[idx] = -1;
	}
	createDirect3DDevice();
	settingDirectInput();
}

NMVCamPin::~NMVCamPin() {
	finishDirectInput();

	if (m_capturePickerThread) {
		m_capturePickerThread->join();
		delete m_capturePickerThread;
		m_capturePickerThread = nullptr;
	}
	if (m_bufferTexture) {
		m_bufferTexture->Release();
		m_bufferTexture = nullptr;
	}
	if (m_Bitmap) {
		DeleteObject(m_Bitmap);
		m_Bitmap = NULL;
	}
	if (m_Hdc) {
		DeleteDC(m_Hdc);
		m_Hdc = NULL;
	}
	if (m_BmpData) {
		delete m_BmpData;
		m_BmpData = NULL;
	}
	if (m_frameBits) {
		delete[] m_frameBits;
		m_frameBits = nullptr;
	}
	if (m_pixelPosX) {
		delete[] m_pixelPosX;
		m_pixelPosX = nullptr;
	}
	if (m_pixelPosY) {
		delete[] m_pixelPosY;
		m_pixelPosY = nullptr;
	}
}

HRESULT	NMVCamPin::OnThreadDestroy() {
	stopCapture();
	return NOERROR;
}

/****************************************************************/
/*  winRT GraphicsCapture Function Start                        */
/****************************************************************/
template<typename T>
auto getDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const &object) {
	auto access = object.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
	com_ptr<T> result;
	check_hresult(access->GetInterface(guid_of<T>(), result.put_void()));
	return result;
}

void NMVCamPin::createDirect3DDevice() {
	UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CPU_ACCESS_READ;
#ifdef _DEBUG
	createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	if (m_dxDevice != nullptr) {
		m_dxDevice.Close();
	}
	com_ptr<ID3D11Device> d3dDevice = nullptr;
	check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
		nullptr, createDeviceFlags, nullptr, 0, D3D11_SDK_VERSION,
		d3dDevice.put(), nullptr, nullptr));
	com_ptr<IDXGIDevice> dxgiDevice = d3dDevice.as<IDXGIDevice>();
	com_ptr<::IInspectable> device = nullptr;
	check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), device.put()));
	m_dxDevice = device.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	d3dDevice->GetImmediateContext(m_deviceCtx.put());

	//CPUから読みだすためのバッファテクスチャ
	m_bufferTextureDesc.Width = MAX_CAP_WIDTH;
	m_bufferTextureDesc.Height = MAX_CAP_HEIGHT;
	m_bufferTextureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	m_bufferTextureDesc.ArraySize = 1;
	m_bufferTextureDesc.BindFlags = 0;
	m_bufferTextureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	m_bufferTextureDesc.MipLevels = 1;
	m_bufferTextureDesc.MiscFlags = 0;
	m_bufferTextureDesc.SampleDesc.Count = 1;
	m_bufferTextureDesc.SampleDesc.Quality = 0;
	m_bufferTextureDesc.Usage = D3D11_USAGE_STAGING;
	d3dDevice->CreateTexture2D(&m_bufferTextureDesc, 0, &m_bufferTexture);
}

void NMVCamPin::openCaptureWindowPicker() {
	init_apartment();
	
	m_attatchedWindow = CreateWindowW(L"STATIC", L"NMUniversalVCam", SS_WHITERECT,
		0, 0, 300, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
	ShowWindow(m_attatchedWindow, SW_SHOW);
	UpdateWindow(m_attatchedWindow);
	//ピッカーを開くために作ったウィンドウをピッカーの選択肢から除外する。
	SetWindowDisplayAffinity(m_attatchedWindow, WDA_EXCLUDEFROMCAPTURE);
	updateAttatchedWindow();

	GraphicsCapturePicker picker;
	auto interop = picker.as<::IInitializeWithWindow>();
	m_pickerResult = interop->Initialize(m_attatchedWindow);
	m_graphicsCaptureAsyncResult = picker.PickSingleItemAsync();
	while (m_isSelectingWindow) {
		updateAttatchedWindow();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	updateAttatchedWindow();
}

void NMVCamPin::updateAttatchedWindow() {
	MSG msg;
	if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

void NMVCamPin::stopCapture() {
	if (isCapturing()) {
		m_frameArrived.revoke();
		m_captureSession = nullptr;
		m_framePool.Close();
		m_framePool = nullptr;
		m_graphicsCaptureItem = nullptr;
	}
}

void NMVCamPin::changeWindow(GraphicsCaptureItem targetCaptureItem) {
	DestroyWindow(m_attatchedWindow);
	m_attatchedWindow = NULL;
	m_graphicsCaptureAsyncResult = nullptr;
	if (targetCaptureItem == nullptr) {
		return;
	}
	stopCapture();

	m_graphicsCaptureItem = targetCaptureItem;
	m_capWinSize = m_graphicsCaptureItem.Size();
	m_capWinSizeInTexture.right = m_capWinSize.Width;
	m_capWinSizeInTexture.bottom = m_capWinSize.Height;
	changePixelPos();
	m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(m_dxDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_capWinSize);
	m_frameArrived = m_framePool.FrameArrived(auto_revoke, { this, &NMVCamPin::onFrameArrived });
	m_captureSession = m_framePool.CreateCaptureSession(m_graphicsCaptureItem);
	//IsCursorCaptureEnabledでカーソルもキャプチャするか指定できる。
	m_captureSession.IsCursorCaptureEnabled(false);
	m_captureSession.StartCapture();
}

void NMVCamPin::convertFrameToBits() {
	D3D11_MAPPED_SUBRESOURCE mapd;
	HRESULT hr;
	hr = m_deviceCtx->Map(m_bufferTexture, 0, D3D11_MAP_READ, 0, &mapd);
	const unsigned char *source = static_cast<const unsigned char *>(mapd.pData);
	int texBitPos = 0;

	//取得したピクセル情報からビットマップを作る処理
	int pixelPosition = 0;
	int bitPosition = 0;
	int texPixelXInt = 0;
	int texPixelYInt = 0;
	double texPixelXDecimal = 0.0;
	double texPixelYDecimal = 0.0;
	double pixelColor[3] = { 0.0, 0.0, 0.0 };
	double pixelRate = 0.0;
	for (int y = 0; y < WINDOW_HEIGHT; y++) {
		for (int x = 0; x < WINDOW_WIDTH; x++) {
			//ピンに送られるビットはBGRで、上下反転するっぽい
			for (int cIdx = 0; cIdx < 3; cIdx++) {
				pixelColor[cIdx] = 0;
			}
			if (m_pixelPosX[pixelPosition] >= 0 && m_pixelPosY[pixelPosition] >= 0) {
				//周囲4ピクセルの情報を使用して、位置で重みづけ平均化したピクセルカラーを適用する。
				texPixelXInt = (int)m_pixelPosX[pixelPosition];
				texPixelYInt = (int)m_pixelPosY[pixelPosition];
				texPixelXDecimal = m_pixelPosX[pixelPosition] - texPixelXInt;
				texPixelYDecimal = m_pixelPosY[pixelPosition] - texPixelYInt;
				for (int px = 0; px < 2; px++) {
					for (int py = 0; py < 2; py++) {
						texBitPos = mapd.RowPitch * (texPixelYInt + py) + 4 * (texPixelXInt + px);
						pixelRate = (px == 0 ? (1.0 - texPixelXDecimal) : texPixelXDecimal) *  (py == 0 ? (1.0 - texPixelYDecimal) : texPixelYDecimal);
						for (int cIdx = 0; cIdx < 3; cIdx++) {
							pixelColor[cIdx] += source[texBitPos + cIdx] * pixelRate;
						}
					}
				}
			}
			for (int cIdx = 0; cIdx < 3; cIdx++) {
				m_frameBits[bitPosition + cIdx] = (unsigned char)(pixelColor[cIdx] + 0.5);
			}
			pixelPosition++;
			bitPosition += 3;
		}
	}
	m_deviceCtx->Unmap(m_bufferTexture, 0);
}

void NMVCamPin::changePixelPos() {
	double widthZoomRate = (double)WINDOW_WIDTH / (double)m_capWinSize.Width;
	double heightZoomRate = (double)WINDOW_HEIGHT / (double)m_capWinSize.Height;
	double zoomRate = (widthZoomRate < heightZoomRate ? widthZoomRate : heightZoomRate);
	double invZoomRate = 1.0 / zoomRate;
	double halfWinWidth = (double)WINDOW_WIDTH * 0.5;
	double halfWinHeight = (double)WINDOW_HEIGHT * 0.5;

	double winSizeWidthDouble = (double)m_capWinSize.Width;
	double winSizeHeightDouble = (double)m_capWinSize.Height;
	double halfCapWidth = winSizeWidthDouble * 0.5;
	double halfCapHeight = winSizeHeightDouble * 0.5;

	int pixeldx = 0;
	double capPosX = 0;
	double capPosY = 0;
	for (int y = 0; y < WINDOW_HEIGHT; y++) {
		for (int x = 0; x < WINDOW_WIDTH; x++) {
			if (m_reverseOutput) {
				capPosX = (halfWinWidth - x) * invZoomRate + halfCapWidth;
			}
			else {
				capPosX = (x - halfWinWidth) * invZoomRate + halfCapWidth;
			}
			capPosY = winSizeHeightDouble - 1.0 - ((y - halfWinHeight) * invZoomRate + halfCapHeight);

			if (capPosX >= 0.0 && capPosX < winSizeWidthDouble && capPosY >= 0 && capPosY < winSizeHeightDouble) {
				m_pixelPosX[pixeldx] = capPosX;
				m_pixelPosY[pixeldx] = capPosY;
			}
			else {
				m_pixelPosX[pixeldx] = -1.0;
				m_pixelPosY[pixeldx] = -1.0;
			}
			pixeldx++;
		}
	}
}

void NMVCamPin::onFrameArrived(Direct3D11CaptureFramePool const &sender,
	winrt::Windows::Foundation::IInspectable const &args)
{
	auto frame = sender.TryGetNextFrame();

	SizeInt32 itemSize = frame.ContentSize();
	if (itemSize.Width <= 0) {
		itemSize.Width = 1;
	}
	if (itemSize.Height <= 0) {
		itemSize.Height = 1;
	}
	if (itemSize.Width != m_capWinSize.Width || itemSize.Height != m_capWinSize.Height) {
		m_capWinSize.Width = itemSize.Width;
		m_capWinSize.Height = itemSize.Height;
		m_capWinSizeInTexture.right = m_capWinSize.Width;
		m_capWinSizeInTexture.bottom = m_capWinSize.Height;
		changePixelPos();
		m_framePool.Recreate(m_dxDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_capWinSize);
	}

	com_ptr<ID3D11Texture2D> texture2D = getDXGIInterfaceFromObject<::ID3D11Texture2D>(frame.Surface());

	//CPU読み込み可能なバッファテクスチャにGPU上でデータコピー
	m_deviceCtx->CopySubresourceRegion(m_bufferTexture, 0, 0, 0, 0,
		texture2D.get(), 0, &m_capWinSizeInTexture);

	convertFrameToBits();
}

/****************************************************************/
/*  winRT GraphicsCapture Function End                          */
/****************************************************************/

/****************************************************************/
/*  DirectInput Function Start                                  */
/****************************************************************/

void NMVCamPin::settingDirectInput() {
	HINSTANCE hInst = GetModuleHandle(NULL);
	HRESULT ret;

	//IDirectInput8の作成
	ret = DirectInput8Create(hInst, DIRECTINPUT_VERSION, IID_IDirectInput8,
		(LPVOID *)&m_lpDI, NULL);
	if (FAILED(ret)) {
		finishDirectInput();
		return;
	}

	//IDirectInputDevice8の作成
	ret = m_lpDI->CreateDevice(GUID_SysKeyboard, &m_lpKeyboard, NULL);
	if (FAILED(ret)) {
		finishDirectInput();
		return;
	}

	//IDirectInput8への入力形式の設定（デフォルト）
	ret = m_lpKeyboard->SetDataFormat(&c_dfDIKeyboard);
	if (FAILED(ret)) {
		finishDirectInput();
		return;
	}
	m_lpKeyboard->Acquire();
}

void NMVCamPin::updateKeyboardState() {
	HRESULT ret;
	ZeroMemory(m_key, sizeof(m_key));
	ret = m_lpKeyboard->GetDeviceState(sizeof(m_key), m_key);
	if (FAILED(ret)) {
		m_lpKeyboard->Acquire();
		m_lpKeyboard->GetDeviceState(sizeof(m_key), m_key);
	}
}

void NMVCamPin::finishDirectInput() {
	if (m_lpKeyboard != NULL) {
		m_lpKeyboard->Release();
		m_lpKeyboard = NULL;
	}
	if (m_lpDI != NULL) {
		m_lpDI->Release();
		m_lpDI = NULL;
	}
}

/****************************************************************/
/*  DirectInput Function End                                    */
/****************************************************************/

/****************************************************************/
/*  Command management Start                                    */
/****************************************************************/

void NMVCamPin::manageCaptureWindowPicker() {
	//pickerが開いており、選択が終わったのを検知したら、そのウィンドウを設定
	//開いていない状態でspace + shiftを押すとウィンドウのピッカーを生成。
	if (m_isSelectingWindow) {
		if (m_graphicsCaptureAsyncResult) {
			winrt::Windows::Foundation::AsyncStatus status = m_graphicsCaptureAsyncResult.Status();
			if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
				GraphicsCaptureItem tmpTarget = m_graphicsCaptureAsyncResult.GetResults();
				changeWindow(tmpTarget);
				m_isSelectingWindow = false;
				m_capturePickerThread->join();
				delete m_capturePickerThread;
				m_capturePickerThread = nullptr;
			}
		}
		m_pickerActivate = true;
	}
	else {
		if ((m_key[DIK_SPACE] & 0x80) != 0
			&& ((m_key[DIK_LSHIFT] & 0x80) != 0 || (m_key[DIK_RSHIFT] & 0x80) != 0)) {
			if (!m_pickerActivate) {
				m_isSelectingWindow = true;
				m_capturePickerThread = new std::thread([](NMVCamPin *inst) {inst->openCaptureWindowPicker(); }, this);

			}
			m_pickerActivate = true;
		}
		else {
			m_pickerActivate = false;
		}
	}
}

void NMVCamPin::manageReverseCommand() {
	//space + ctrlでキャプチャ画面の左右反転
	if ((m_key[DIK_SPACE] & 0x80) != 0
		&& ((m_key[DIK_LCONTROL] & 0x80) != 0 || (m_key[DIK_RCONTROL] & 0x80) != 0)) {
		if (!m_previousChangeReverseOutput) {
			m_reverseOutput = !m_reverseOutput;
			changePixelPos();
			m_previousChangeReverseOutput = true;
		}
	}
	else {
		m_previousChangeReverseOutput = false;
	}
}

/****************************************************************/
/*  Command management End                                      */
/****************************************************************/

STDMETHODIMP NMVCamPin::Notify(IBaseFilter *pSelf, Quality q) {
	return E_NOTIMPL;
}

HRESULT NMVCamPin::GetMediaType(CMediaType *pMediaType) {
	HRESULT hr=NOERROR;
	VIDEOINFO *pvi=(VIDEOINFO *)pMediaType->AllocFormatBuffer(sizeof(VIDEOINFO));
	ZeroMemory(pvi, sizeof(VIDEOINFO));

	pvi->AvgTimePerFrame=m_rtFrameLength;

	BITMAPINFOHEADER *pBmi=&(pvi->bmiHeader);
	pBmi->biSize=sizeof(BITMAPINFOHEADER);
	pBmi->biWidth = WINDOW_WIDTH;
	pBmi->biHeight = WINDOW_HEIGHT;
	pBmi->biPlanes=1;
	pBmi->biBitCount=PIXEL_BIT;
	pBmi->biCompression=BI_RGB;
	pvi->bmiHeader.biSizeImage=DIBSIZE(pvi->bmiHeader);

	SetRectEmpty(&(pvi->rcSource));
	SetRectEmpty(&(pvi->rcTarget));

	pMediaType->SetType(&MEDIATYPE_Video);
	pMediaType->SetFormatType(&FORMAT_VideoInfo);

	const GUID subtype=GetBitmapSubtype(&pvi->bmiHeader);
	pMediaType->SetSubtype(&subtype);

	pMediaType->SetTemporalCompression(FALSE);
	const int bmpsize=DIBSIZE(*pBmi);
	pMediaType->SetSampleSize(bmpsize);
	if(m_BmpData) delete m_BmpData;
	m_BmpData=new DWORD[pBmi->biWidth * pBmi->biHeight];
	memset(m_BmpData,0,pMediaType->GetSampleSize());
	
	HDC dwhdc=GetDC(GetDesktopWindow());
	m_Bitmap=
		CreateDIBitmap(dwhdc, pBmi, CBM_INIT, m_BmpData, (BITMAPINFO*)pBmi, DIB_RGB_COLORS);
	
	if (m_Hdc) {
		DeleteDC(m_Hdc);
		m_Hdc = NULL;
	}
	m_Hdc=CreateCompatibleDC(dwhdc);
	SelectObject(m_Hdc, m_Bitmap);
	ReleaseDC(GetDesktopWindow(), dwhdc);
	
	return hr;
}

HRESULT NMVCamPin::CheckMediaType(const CMediaType *pMediaType) {
	HRESULT hr=NOERROR;
	CheckPointer(pMediaType,E_POINTER);
	CMediaType mt;
	GetMediaType(&mt);
	if(mt==*pMediaType) {
		FreeMediaType(mt);
		return S_OK;
	}
	FreeMediaType(mt);
	return E_FAIL;
}

HRESULT NMVCamPin::DecideBufferSize(IMemAllocator *pAlloc,ALLOCATOR_PROPERTIES *pRequest) {
	HRESULT hr=NOERROR;
	VIDEOINFO *pvi=reinterpret_cast<VIDEOINFO*>(m_mt.Format());
	ASSERT(pvi != NULL);
	pRequest->cBuffers=1;
	// バッファサイズはビットマップ1枚分以上である必要がある
	if(pvi->bmiHeader.biSizeImage > (DWORD)pRequest->cbBuffer) {
		pRequest->cbBuffer=pvi->bmiHeader.biSizeImage;
	}
	// アロケータプロパティを設定しなおす
	ALLOCATOR_PROPERTIES Actual;
	hr=pAlloc->SetProperties(pRequest, &Actual);
	if(FAILED(hr)) {
		return hr;
	}
	if(Actual.cbBuffer < pRequest->cbBuffer) {
		return E_FAIL;
	}

	return S_OK;
}

HRESULT NMVCamPin::FillBuffer(IMediaSample *pSample) {
	HRESULT hr=E_FAIL;
	CheckPointer(pSample,E_POINTER);
	// ダウンストリームフィルタが
	// フォーマットを動的に変えていないかチェック
	ASSERT(m_mt.formattype == FORMAT_VideoInfo);
	ASSERT(m_mt.cbFormat >= sizeof(VIDEOINFOHEADER));
	// フレームに書き込み
	LPBYTE pSampleData=NULL;
	const long size=pSample->GetSize();
	pSample->GetPointer(&pSampleData);

	TCHAR buffer[200];
	CRefTime ref;
	m_pFilter->StreamTime(ref);
	
	updateKeyboardState();
	
	manageCaptureWindowPicker();

	manageReverseCommand();

	//キャプチャされた画像のビット列をpSampleDataにコピー
	if (isCapturing()) {
		memcpy(pSampleData, m_frameBits, WINDOW_WIDTH * WINDOW_HEIGHT * ((PIXEL_BIT - 1) / 8 + 1));
	}
	else {
		SelectObject(m_Hdc, m_brush);
		PatBlt(m_Hdc, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, PATCOPY);
		if (m_pickerResult == S_OK) {
			_snwprintf_s(buffer, _countof(buffer), _TRUNCATE, TEXT("No Signal: Press SHIFT + SPACE to select a window. "));
		}
		else {
			_snwprintf_s(buffer, _countof(buffer), _TRUNCATE, TEXT("Error occurs. "));
		}
		TextOut(m_Hdc, 0, 0, buffer, lstrlen(buffer));
		VIDEOINFO *pvi = (VIDEOINFO *)m_mt.Format();
		GetDIBits(m_Hdc, m_Bitmap, 0, WINDOW_HEIGHT,
			pSampleData, (BITMAPINFO*)&pvi->bmiHeader, DIB_RGB_COLORS);
	}

	const REFERENCE_TIME delta=m_rtFrameLength;
	REFERENCE_TIME start_time=ref;
	FILTER_STATE state;
	m_pFilter->GetState(0, &state);
	if(state==State_Paused)
		start_time=0;
	REFERENCE_TIME end_time=(start_time+delta);
	pSample->SetTime(&start_time, &end_time);
	pSample->SetActualDataLength(size);
	pSample->SetSyncPoint(TRUE);

	//CPU使用率を抑えて、ZoomなどのUIの反応をしやすくするために適度にSleepする。
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	return S_OK;
}


