#include "stdafx.h"
#include <d3dcompiler.h>

using namespace winrt;
using namespace winrt::Windows::System;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Graphics::Capture;

#define HLSL_EXTERNAL_INCLUDE(...) #__VA_ARGS__

// Embeded hlsl shader source code.
const char* hlslOffscreenRenderingCode =
#include "SpriteShader.hlsl"
;

const char* hlslFormatterCode =
#include "SampleFormatter.hlsl"
;

NMVCamPin::NMVCamPin(HRESULT *phr, NMVCamSource *pFilter) : CSourceStream(NAME("NMVCamPin"), phr, pFilter, OUTPUT_PIN_NAME)
	,_pFilter(pFilter)
	,_isSelectingWindow(false), _pickerActivate(false), _reverseOutput(false), _previousChangeReverseOutput(false)
	,_deviceCtx(nullptr), _bufferTexture(nullptr), _attatchedWindow(NULL)
	,_graphicsCaptureItem(nullptr), _framePool(nullptr)
	,_captureSession(nullptr)
	,_lpDI(NULL), _lpKeyboard(NULL), _capturePickerThread(nullptr)
	,_pickerResult(S_OK), _rtFrameLength(666666)
{
	GetMediaType(&m_mt);

	//CPU読み出し可能なバッファをGPU上に作成
	_capWinSize.Width = 1;
	_capWinSize.Height = 1;
	_capWinSizeInTexture.left = 0;
	_capWinSizeInTexture.right = 1;
	_capWinSizeInTexture.top = 0;
	_capWinSizeInTexture.bottom = 1;
	_capWinSizeInTexture.front = 0;
	_capWinSizeInTexture.back = 1;

	createDirect3DDevice();
	settingDirectInput();
	setupOffscreenRendering();
	setupSampleFormatter();
	setupPlaceholder();
}

NMVCamPin::~NMVCamPin() {
	finishDirectInput();

	if (_capturePickerThread) {
		_capturePickerThread->join();
		delete _capturePickerThread;
		_capturePickerThread = nullptr;
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
	if (_dxWinRTDevice != nullptr) {
		_dxWinRTDevice.Close();
	}
	com_ptr<ID3D11Device> d3dDevice = nullptr;
	check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
		nullptr, createDeviceFlags, nullptr, 0, D3D11_SDK_VERSION,
		_dxDevice.put(), nullptr, nullptr));
	com_ptr<IDXGIDevice> dxgiDevice = _dxDevice.as<IDXGIDevice>();
	com_ptr<::IInspectable> device = nullptr;
	check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), device.put()));
	_dxWinRTDevice = device.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	_dxDevice->GetImmediateContext(_deviceCtx.put());

	D3D11_TEXTURE2D_DESC textureDesc;
	//共通
	textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	textureDesc.ArraySize = 1;
	textureDesc.MipLevels = 1;
	textureDesc.MiscFlags = 0;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;

	//キャプチャしてきたテクスチャ
	textureDesc.Width = MAX_CAP_WIDTH;
	textureDesc.Height = MAX_CAP_HEIGHT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	textureDesc.CPUAccessFlags = 0;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	_dxDevice->CreateTexture2D(&textureDesc, 0, _captureWindowTexture.put());

	//オフスクリーンレンダリングの描画先テクスチャ
	textureDesc.Width = WINDOW_WIDTH;
	textureDesc.Height = WINDOW_HEIGHT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	textureDesc.CPUAccessFlags = 0;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	_dxDevice->CreateTexture2D(&textureDesc, 0, _offscreenRenderingTexture.put());

	//CPUから読みだすためのバッファテクスチャ
	textureDesc.Width = WINDOW_WIDTH;
	textureDesc.Height = WINDOW_HEIGHT;
	textureDesc.BindFlags = 0;
	textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	textureDesc.Usage = D3D11_USAGE_STAGING;
	_dxDevice->CreateTexture2D(&textureDesc, 0, _bufferTexture.put());
}

void NMVCamPin::openCaptureWindowPicker() {
	init_apartment();
	
	_attatchedWindow = CreateWindowW(L"STATIC", L"NMUniversalVCam", SS_WHITERECT,
		0, 0, 300, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
	ShowWindow(_attatchedWindow, SW_SHOW);
	UpdateWindow(_attatchedWindow);
	//ピッカーを開くために作ったウィンドウをピッカーの選択肢から除外する。
	SetWindowDisplayAffinity(_attatchedWindow, WDA_EXCLUDEFROMCAPTURE);
	updateAttatchedWindow();

	GraphicsCapturePicker picker;
	auto interop = picker.as<::IInitializeWithWindow>();
	_pickerResult = interop->Initialize(_attatchedWindow);
	_graphicsCaptureAsyncResult = picker.PickSingleItemAsync();
	while (_isSelectingWindow) {
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
		_frameArrived.revoke();
		_captureSession = nullptr;
		_framePool.Close();
		_framePool = nullptr;
		_graphicsCaptureItem = nullptr;
	}
}

void NMVCamPin::changeWindow(GraphicsCaptureItem targetCaptureItem) {
	DestroyWindow(_attatchedWindow);
	_attatchedWindow = NULL;
	_graphicsCaptureAsyncResult = nullptr;
	if (targetCaptureItem == nullptr) {
		return;
	}
	stopCapture();

	_graphicsCaptureItem = targetCaptureItem;
	_capWinSize = _graphicsCaptureItem.Size();
	_capWinSizeInTexture.right = _capWinSize.Width;
	_capWinSizeInTexture.bottom = _capWinSize.Height;
	_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(_dxWinRTDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, _capWinSize);
	_frameArrived = _framePool.FrameArrived(auto_revoke, { this, &NMVCamPin::onFrameArrived });
	_captureSession = _framePool.CreateCaptureSession(_graphicsCaptureItem);
	//IsCursorCaptureEnabledでカーソルもキャプチャするか指定できる。
	_captureSession.IsCursorCaptureEnabled(false);
	_captureSession.StartCapture();
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
	if (itemSize.Width != _capWinSize.Width || itemSize.Height != _capWinSize.Height) {
		_capWinSize.Width = itemSize.Width;
		_capWinSize.Height = itemSize.Height;
		_capWinSizeInTexture.right = _capWinSize.Width;
		_capWinSizeInTexture.bottom = _capWinSize.Height;
		_framePool.Recreate(_dxWinRTDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, _capWinSize);
	}

	com_ptr<ID3D11Texture2D> texture2D = getDXGIInterfaceFromObject<::ID3D11Texture2D>(frame.Surface());

	//_sharedCaptureWindowTextureにGPU上でデータコピー
	_deviceCtx->CopySubresourceRegion(_captureWindowTexture.get(), 0, 0, 0, 0,
		texture2D.get(), 0, &_capWinSizeInTexture);
	_deviceCtx->Flush();

}

/****************************************************************/
/*  winRT GraphicsCapture Function End                          */
/****************************************************************/

/****************************************************************/
/*  DirectX Function Start                                      */
/****************************************************************/

// _offscreenRenderingTextureへのオフスクリーンレンダリングの準備
void NMVCamPin::setupOffscreenRendering() {
	DXGI_FORMAT dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

	CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(D3D11_RTV_DIMENSION_TEXTURE2D, dxgiFormat);
	_dxDevice->CreateRenderTargetView(_offscreenRenderingTexture.get(),
		&renderTargetViewDesc, _renderTargetView.put());

	CD3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc(D3D11_SRV_DIMENSION_TEXTURE2D, dxgiFormat);
	_dxDevice->CreateShaderResourceView(_captureWindowTexture.get(),
		&shaderResourceViewDesc, _shaderResourceView.put());

	D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT, 0.0f, 1.0f };
	_deviceCtx->RSSetViewports(1, &vp);

	size_t hlslSize = std::strlen(hlslOffscreenRenderingCode);
	com_ptr<ID3DBlob> compiledVS;
	D3DCompile(hlslOffscreenRenderingCode, hlslSize, nullptr, nullptr, nullptr,
		"VS", "vs_5_0", 0, 0, compiledVS.put(), nullptr);

	com_ptr<ID3DBlob> compiledPS;
	D3DCompile(hlslOffscreenRenderingCode, hlslSize, nullptr, nullptr, nullptr,
		"PS", "ps_5_0", 0, 0, compiledPS.put(), nullptr);

	_dxDevice->CreateVertexShader(compiledVS->GetBufferPointer(),
		compiledVS->GetBufferSize(), nullptr, _spriteVS.put());
	_deviceCtx->VSSetShader(_spriteVS.get(), 0, 0);

	_dxDevice->CreatePixelShader(compiledPS->GetBufferPointer(),
		compiledPS->GetBufferSize(), nullptr, _spritePS.put());
	_deviceCtx->PSSetShader(_spritePS.get(), 0, 0);

	D3D11_INPUT_ELEMENT_DESC layout[2] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXUV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};

	_dxDevice->CreateInputLayout(layout, 2, compiledVS->GetBufferPointer(),
		compiledVS->GetBufferSize(), _spriteInputLayout.put());
	_deviceCtx->IASetInputLayout(_spriteInputLayout.get());

	_vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	_vbDesc.ByteWidth = sizeof(VertexType) * 4;
	_vbDesc.MiscFlags = 0;
	_vbDesc.StructureByteStride = 0;
	_vbDesc.Usage = D3D11_USAGE_DEFAULT;
	_vbDesc.CPUAccessFlags = 0;

	D3D11_SUBRESOURCE_DATA initData = {
		_polygonVertex, sizeof(_polygonVertex), 0
	};

	_dxDevice->CreateBuffer(&_vbDesc, &initData, _vertexBuffer.put());

	UINT stride = sizeof(VertexType);
	UINT offset = 0;
	ID3D11Buffer* tempBufferPtr = _vertexBuffer.get();
	_deviceCtx->IASetVertexBuffers(0, 1, &tempBufferPtr, &stride, &offset);
	_deviceCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
}

// SampleFormatter実行の準備
void NMVCamPin::setupSampleFormatter()
{
	CD3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc(D3D11_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_B8G8R8A8_UNORM);
	_dxDevice->CreateShaderResourceView(_offscreenRenderingTexture.get(),
		&shaderResourceViewDesc, _formatterSRV.put());

	UINT bufferByteSize = WINDOW_WIDTH * WINDOW_HEIGHT * PIXEL_BYTE;

	size_t hlslSize = std::strlen(hlslFormatterCode);
	std::string csThreadsStr = std::to_string(CS_THREADS_NUM);
	std::string windowWidthStr = std::to_string(WINDOW_WIDTH);
	com_ptr<ID3DBlob> compiledCS;
	D3D_SHADER_MACRO csMacro[] = {
		"CS_THREADS_NUM_IN_CS", csThreadsStr.c_str(),
		"WINDOW_WIDTH_IN_CS", windowWidthStr.c_str(),
		NULL, NULL
	};
	D3DCompile(hlslFormatterCode, hlslSize, nullptr, csMacro, nullptr,
		"formatterMain", "cs_5_0", 0, 0, compiledCS.put(), nullptr);

	_dxDevice->CreateComputeShader(compiledCS->GetBufferPointer(),
		compiledCS->GetBufferSize(), nullptr, _formatterCS.put());
	_deviceCtx->CSSetShader(_formatterCS.get(), 0, 0);

	D3D11_BUFFER_DESC bufferDesc;
	bufferDesc.ByteWidth = bufferByteSize;
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	bufferDesc.CPUAccessFlags = 0;
	bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	_dxDevice->CreateBuffer(&bufferDesc, nullptr, _gpuFormatterBuffer.put());

	bufferDesc.Usage = D3D11_USAGE_STAGING;
	bufferDesc.BindFlags = 0;
	bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	bufferDesc.MiscFlags = 0;
	_dxDevice->CreateBuffer(&bufferDesc, nullptr, _cpuSampleBuffer.put());

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.NumElements = bufferByteSize / 4;
	uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	_dxDevice->CreateUnorderedAccessView(_gpuFormatterBuffer.get(), &uavDesc, _formatterUAV.put());

	ID3D11UnorderedAccessView* uavs[] = { _formatterUAV.get() };
	UINT initialCounts[] = { 0 };
	_deviceCtx->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);
}

// "No Signal"表示の準備
void NMVCamPin::setupPlaceholder() {
	D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, _placeholderD2DFactory.put());
	D2D1_RENDER_TARGET_PROPERTIES d2d1props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

	com_ptr<IDXGISurface>renderTargetSurface;
	_offscreenRenderingTexture.as(IID_PPV_ARGS(renderTargetSurface.put()));

	HRESULT hr = _placeholderD2DFactory->CreateDxgiSurfaceRenderTarget(
		renderTargetSurface.get(), &d2d1props, _placeholderRenderTarget.put());

	_placeholderRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

	DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
		reinterpret_cast<IUnknown**>(_placeholderDWFactory.put()));

	_placeholderDWFactory->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL, 40, L"", _placeholderTextFormat.put());

	_placeholderTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	_placeholderTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

	_placeholderRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), _placeholderBrush.put());

	// 上下反転
	D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Scale(1.0f, -1.0f,
		D2D1::Point2F(WINDOW_WIDTH * 0.5f, WINDOW_HEIGHT * 0.5f));
	_placeholderRenderTarget->SetTransform(transform);
}

//キャプチャウィンドウテクスチャのオフスクリーンレンダリング
void NMVCamPin::drawCaptureWindow()
{
	ID3D11RenderTargetView* tempRenderTargetViewPtr = _renderTargetView.get();
	_deviceCtx->OMSetRenderTargets(1, &tempRenderTargetViewPtr, nullptr);

	float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	_deviceCtx->ClearRenderTargetView(_renderTargetView.get(), color);

	// ポリゴン頂点位置の計算
	float xPosRate = 1.0f;
	float yPosRate = 1.0f;
	float rectRate = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
	float floatWindowWidth = static_cast<float>(_capWinSize.Width);
	float floatWindowHeight = static_cast<float>(_capWinSize.Height);

	if (floatWindowWidth > floatWindowHeight * rectRate) {
		yPosRate = floatWindowHeight * rectRate / floatWindowWidth;
	}
	else {
		xPosRate = floatWindowWidth / (floatWindowHeight * rectRate);
	}

	// ポリゴン頂点のテクスチャ座標計算
	float widthTextureRate = floatWindowWidth / static_cast<float>(MAX_CAP_WIDTH);
	float heightTextureRate = floatWindowHeight / static_cast<float>(MAX_CAP_HEIGHT);

	//ピンに送られるビットはBGRで、上下反転するっぽい
	_polygonVertex[0] = { {-xPosRate, yPosRate, 0}, {0, heightTextureRate} };
	_polygonVertex[1] = { {xPosRate, yPosRate, 0}, {widthTextureRate, heightTextureRate} };
	_polygonVertex[2] = { {-xPosRate, -yPosRate, 0}, {0, 0} };
	_polygonVertex[3] = { {xPosRate, -yPosRate, 0}, {widthTextureRate, 0} };

	// 左右反転
	if (_reverseOutput) {
		_polygonVertex[0].Tex.x = widthTextureRate;
		_polygonVertex[1].Tex.x = 0;
		_polygonVertex[2].Tex.x = widthTextureRate;
		_polygonVertex[3].Tex.x = 0;
	}

	_deviceCtx->UpdateSubresource(_vertexBuffer.get(), 0, nullptr, _polygonVertex, 0, 0);

	ID3D11ShaderResourceView* tempShaderResourceViewPtr[] = { _shaderResourceView.get() };
	_deviceCtx->PSSetShaderResources(0, 1, tempShaderResourceViewPtr);

	_deviceCtx->Draw(4, 0);
	_deviceCtx->Flush();

	_deviceCtx->OMSetRenderTargets(0, nullptr, nullptr);
}

// DirectWriteで"No Signal"の表示
void NMVCamPin::drawPlaceholder() {
	_placeholderRenderTarget->BeginDraw();

	_placeholderRenderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));

	std::wstring placeholderText(L"No Signal: \nPress SHIFT + SPACE to select a window.");
	D2D1_RECT_F rect;
	rect.left = 0;
	rect.right = WINDOW_WIDTH - 1;
	rect.top = 0;
	rect.bottom = WINDOW_HEIGHT - 1;
	_placeholderRenderTarget->DrawText(placeholderText.c_str(), placeholderText.size(),
		_placeholderTextFormat.get(), rect, _placeholderBrush.get());

	_placeholderRenderTarget->EndDraw();
}

// コンピュートシェーダでサンプルのフォーマットにあったバッファを作成
void NMVCamPin::getSampleOnCaptureWindow(LPBYTE sampleData)
{
	ID3D11ShaderResourceView* tempShaderResourceViewPtr[] = { _formatterSRV.get() };
	_deviceCtx->CSSetShaderResources(0, 1, tempShaderResourceViewPtr);
	_deviceCtx->Dispatch(WINDOW_WIDTH / (CS_THREADS_NUM * 4), WINDOW_HEIGHT / CS_THREADS_NUM, 1);

	ID3D11ShaderResourceView* tempShaderResourceViewNullPtr[] = { nullptr };
	_deviceCtx->CSSetShaderResources(0, 1, tempShaderResourceViewNullPtr);

	_deviceCtx->CopyResource(_cpuSampleBuffer.get(), _gpuFormatterBuffer.get());

	com_ptr<IDXGISurface> dxgiSurface;
	_cpuSampleBuffer->QueryInterface(IID_PPV_ARGS(dxgiSurface.put()));

	DXGI_MAPPED_RECT mapFromCpuSampleBuffer;
	dxgiSurface->Map(&mapFromCpuSampleBuffer, DXGI_MAP_READ);

	CopyMemory((PVOID)sampleData, (PVOID)mapFromCpuSampleBuffer.pBits, WINDOW_WIDTH * WINDOW_HEIGHT * PIXEL_BYTE);

	dxgiSurface->Unmap();
}

/****************************************************************/
/*  DirectX Function End                                        */
/****************************************************************/

/****************************************************************/
/*  DirectInput Function Start                                  */
/****************************************************************/

void NMVCamPin::settingDirectInput() {
	HINSTANCE hInst = GetModuleHandle(NULL);
	HRESULT ret;

	//IDirectInput8の作成
	ret = DirectInput8Create(hInst, DIRECTINPUT_VERSION, IID_IDirectInput8,
		(LPVOID *)&_lpDI, NULL);
	if (FAILED(ret)) {
		finishDirectInput();
		return;
	}

	//IDirectInputDevice8の作成
	ret = _lpDI->CreateDevice(GUID_SysKeyboard, &_lpKeyboard, NULL);
	if (FAILED(ret)) {
		finishDirectInput();
		return;
	}

	//IDirectInput8への入力形式の設定（デフォルト）
	ret = _lpKeyboard->SetDataFormat(&c_dfDIKeyboard);
	if (FAILED(ret)) {
		finishDirectInput();
		return;
	}
	_lpKeyboard->Acquire();
}

void NMVCamPin::updateKeyboardState() {
	HRESULT ret;
	ZeroMemory(_key, sizeof(_key));
	ret = _lpKeyboard->GetDeviceState(sizeof(_key), _key);
	if (FAILED(ret)) {
		_lpKeyboard->Acquire();
		_lpKeyboard->GetDeviceState(sizeof(_key), _key);
	}
}

void NMVCamPin::finishDirectInput() {
	if (_lpKeyboard != NULL) {
		_lpKeyboard->Release();
		_lpKeyboard = NULL;
	}
	if (_lpDI != NULL) {
		_lpDI->Release();
		_lpDI = NULL;
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
	if (_isSelectingWindow) {
		if (_graphicsCaptureAsyncResult) {
			winrt::Windows::Foundation::AsyncStatus status = _graphicsCaptureAsyncResult.Status();
			if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
				GraphicsCaptureItem tmpTarget = _graphicsCaptureAsyncResult.GetResults();
				changeWindow(tmpTarget);
				_isSelectingWindow = false;
				_capturePickerThread->join();
				delete _capturePickerThread;
				_capturePickerThread = nullptr;
			}
		}
		_pickerActivate = true;
	}
	else {
		if ((_key[DIK_SPACE] & 0x80) != 0
			&& ((_key[DIK_LSHIFT] & 0x80) != 0 || (_key[DIK_RSHIFT] & 0x80) != 0)) {
			if (!_pickerActivate) {
				_isSelectingWindow = true;
				_capturePickerThread = new std::thread([](NMVCamPin *inst) {inst->openCaptureWindowPicker(); }, this);

			}
			_pickerActivate = true;
		}
		else {
			_pickerActivate = false;
		}
	}
}

void NMVCamPin::manageReverseCommand() {
	//space + ctrlでキャプチャ画面の左右反転
	if ((_key[DIK_SPACE] & 0x80) != 0
		&& ((_key[DIK_LCONTROL] & 0x80) != 0 || (_key[DIK_RCONTROL] & 0x80) != 0)) {
		if (!_previousChangeReverseOutput) {
			_reverseOutput = !_reverseOutput;
			_previousChangeReverseOutput = true;
		}
	}
	else {
		_previousChangeReverseOutput = false;
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

	pvi->AvgTimePerFrame=_rtFrameLength;

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
	_pFilter->StreamTime(ref);
	
	updateKeyboardState();
	
	manageCaptureWindowPicker();

	manageReverseCommand();

	//キャプチャされた画像のビット列をpSampleDataにコピー
	if (isCapturing()) {
		drawCaptureWindow();
	}
	else {
		drawPlaceholder();
	}

	getSampleOnCaptureWindow(pSampleData);

	const REFERENCE_TIME delta=_rtFrameLength;
	REFERENCE_TIME start_time=ref;
	FILTER_STATE state;
	_pFilter->GetState(0, &state);
	if(state==State_Paused)
		start_time=0;
	REFERENCE_TIME end_time=(start_time+delta);
	pSample->SetTime(&start_time, &end_time);
	pSample->SetActualDataLength(size);
	pSample->SetSyncPoint(TRUE);

	//CPU使用率を抑えて、ZoomなどのUIの反応をしやすくするために適度にSleepする。
	std::this_thread::sleep_for(std::chrono::milliseconds(5));

	return S_OK;
}


