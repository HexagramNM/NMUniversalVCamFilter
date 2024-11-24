#include "stdafx.h"

using namespace winrt;
using namespace winrt::Windows::System;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Graphics::Capture;

// {52224B1C-7802-412A-8D36-11FCF0CEF47B}
DEFINE_GUID(CLSID_NMUniversalVCam,
	0x52224b1c, 0x7802, 0x412a, 0x8d, 0x36, 0x11, 0xfc, 0xf0, 0xce, 0xf4, 0x7b);

// ピンタイプの定義
const AMOVIESETUP_MEDIATYPE sudPinTypes =
{
	&MEDIATYPE_Video,
	&MEDIASUBTYPE_RGB24
};
// 入力用、出力用ピンの情報
const AMOVIESETUP_PIN sudPins =
{
	OUTPUT_PIN_NAME,
	FALSE,
	TRUE,
	FALSE,
	FALSE,
	&CLSID_NULL,
	NULL,
	1,
	&sudPinTypes
};

// フィルタ情報
const AMOVIESETUP_FILTER afFilterInfo =
{
	&CLSID_NMUniversalVCam,
	FILTER_NAME,
	MERIT_NORMAL,
	1,
	&sudPins
};

class NMVCamPin;

//ソースフィルタクラス
class NMVCamSource : public CSource
{
private:
	NMVCamPin *_pin;
public:

	NMVCamSource(LPUNKNOWN pUnk, HRESULT *phr);
	virtual ~NMVCamSource();
	static CUnknown * WINAPI CreateInstance(LPUNKNOWN pUnk, HRESULT *phr);
	
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv);
	IFilterGraph *GetGraph() { return m_pGraph; }
};

// プッシュピンクラス
class NMVCamPin : public CSourceStream, public IAMStreamConfig , public IKsPropertySet, public IAMFilterMiscFlags{
public:
	NMVCamPin(HRESULT *phr, NMVCamSource *pFilter);
	virtual			~NMVCamPin();
	STDMETHODIMP	Notify(IBaseFilter *pSelf, Quality q) override;

	// CSourceStreamの実装
	HRESULT			GetMediaType(CMediaType *pMediaType) override;
	HRESULT			CheckMediaType(const CMediaType *pMediaType) override;
	HRESULT			DecideBufferSize(IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *pRequest) override;
	HRESULT			FillBuffer(IMediaSample *pSample) override;
	HRESULT			OnThreadDestroy(void) override;

	//IUnknown
	STDMETHODIMP QueryInterface(REFIID   riid, LPVOID * ppvObj) override;

	STDMETHODIMP_(ULONG) AddRef() override;

	STDMETHODIMP_(ULONG) Release() override;

	//IKsPropertySet
	HRESULT STDMETHODCALLTYPE Get(
		REFGUID PropSet,
		ULONG   Id,
		LPVOID  InstanceData,
		ULONG   InstanceLength,
		LPVOID  PropertyData,
		ULONG   DataLength,
		ULONG   *BytesReturned
	) override;

	HRESULT STDMETHODCALLTYPE Set(
		REFGUID PropSet,
		ULONG   Id,
		LPVOID  InstanceData,
		ULONG   InstanceLength,
		LPVOID  PropertyData,
		ULONG   DataLength
	) override;

	HRESULT STDMETHODCALLTYPE QuerySupported(
		REFGUID PropSet,
		ULONG   Id,
		ULONG   *TypeSupport
	) override;

	//IAMStreamConfig
	HRESULT STDMETHODCALLTYPE GetFormat(
		AM_MEDIA_TYPE **ppmt
	) override;

	HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(
		int *piCount,
		int *piSize
	) override;

	HRESULT STDMETHODCALLTYPE GetStreamCaps(
		int           iIndex,
		AM_MEDIA_TYPE **ppmt,
		BYTE          *pSCC
	) override;

	HRESULT STDMETHODCALLTYPE SetFormat(
		AM_MEDIA_TYPE *pmt
	) override;

	//IAMFilterMiscFlags
	ULONG STDMETHODCALLTYPE GetMiscFlags() override;

protected:
private:
	VIDEOINFOHEADER videoInfo {
		RECT{0, 0, WINDOW_WIDTH, WINDOW_HEIGHT},
		RECT{0, 0, WINDOW_WIDTH, WINDOW_HEIGHT},
		30 * WINDOW_WIDTH * WINDOW_HEIGHT * PIXEL_BIT,
		0,
		160000,
		BITMAPINFOHEADER{
			sizeof(BITMAPINFOHEADER),
			WINDOW_WIDTH,
			WINDOW_HEIGHT,
			1,
			PIXEL_BIT,
			BI_RGB,
			0,
			2500,
			2500,
			0,
			0
		}
	};

	/****************************************************************/
	/*  winRT GraphicsCapture Function                              */
	/****************************************************************/
	void createDirect3DDevice();

	void openCaptureWindowPicker();

	void updateAttatchedWindow();

	bool isCapturing() { return _framePool != nullptr; }

	void stopCapture();

	void changeWindow(GraphicsCaptureItem targetCaptureItem);

	void onFrameArrived(Direct3D11CaptureFramePool const &sender,
						winrt::Windows::Foundation::IInspectable const &args);

	/****************************************************************/
	/*  DirectX Function                                            */
	/****************************************************************/
	void SetupOffscreenRendering();

	void SetupPlaceholder();

	void DrawCaptureWindow();

	void DrawPlaceholder();

	void GetSampleOnCaptureWindow(LPBYTE sampleData);

	/****************************************************************/
	/*  DirectInput Function                                        */
	/****************************************************************/
	void settingDirectInput();

	void updateKeyboardState();

	void finishDirectInput();

	/****************************************************************/
	/*  Command management                                          */
	/****************************************************************/
	void manageCaptureWindowPicker();

	void manageReverseCommand();
	
	NMVCamSource* _pFilter;			//このピンが所属しているフィルタへのポインタ
	bool _isSelectingWindow;
	bool _pickerActivate;
	bool _reverseOutput;
	bool _previousChangeReverseOutput;
	IDirect3DDevice _dxWinRTDevice;
	com_ptr<ID3D11Device> _dxDevice;
	com_ptr<ID3D11DeviceContext> _deviceCtx;
	HWND _attatchedWindow;
	winrt::Windows::Foundation::IAsyncOperation<GraphicsCaptureItem> _graphicsCaptureAsyncResult;
	GraphicsCaptureItem _graphicsCaptureItem;
	Direct3D11CaptureFramePool _framePool;
	event_revoker<IDirect3D11CaptureFramePool> _frameArrived;
	GraphicsCaptureSession _captureSession;
	SizeInt32 _capWinSize;
	D3D11_BOX _capWinSizeInTexture;
	LPDIRECTINPUT8 _lpDI;
	LPDIRECTINPUTDEVICE8 _lpKeyboard;
	BYTE _key[256];
	std::thread *_capturePickerThread;
	HRESULT _pickerResult;

	com_ptr<ID3D11Texture2D> _captureWindowTexture;
	com_ptr<ID3D11Texture2D> _offscreenRenderingTexture;
	com_ptr<ID3D11Texture2D> _bufferTexture;

	struct VertexType
	{
		::DirectX::XMFLOAT3 Pos;
		::DirectX::XMFLOAT2 Tex;
	};

	com_ptr<ID3D11RenderTargetView>_renderTargetView;
	com_ptr<ID3D11ShaderResourceView> _shaderResourceView;
	com_ptr<ID3D11VertexShader> _spriteVS;
	com_ptr<ID3D11PixelShader> _spritePS;
	com_ptr<ID3D11InputLayout> _spriteInputLayout;

	D3D11_BUFFER_DESC _vbDesc;
	VertexType _polygonVertex[4];
	com_ptr<ID3D11Buffer> _vertexBuffer;

	com_ptr<ID2D1Factory> _placeholderD2DFactory;
	com_ptr<ID2D1RenderTarget> _placeholderRenderTarget;
	com_ptr<IDWriteFactory> _placeholderDWFactory;
	com_ptr<IDWriteTextFormat> _placeholderTextFormat;
	com_ptr<ID2D1SolidColorBrush> _placeholderBrush;

	REFERENCE_TIME	_rtFrameLength;	//1フレームあたりの時間
};
