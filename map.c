#include "map_internal.h"
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <strsafe.h>
#include <stdlib.h>
#include <float.h>

#define CACHE_COUNT 256
#define TILE_READY (WM_APP+411)
#define MAP_REG L"Software\\GeoVoice\\Studio\\Map"
typedef struct {
    LONG refs;
    MapVector *vector;
    wchar_t path[MAP_PATH],credit[512];
} Source;
typedef struct {int x,y,z; BOOL valid; HBITMAP bitmap; ULONGLONG used;} Tile;
typedef struct {
    HWND window;
    MapPick pick; void *context;
    double x,y;
    int zoom,selected;
    MapPoint *points;
    size_t count;
    BOOL lines,labels,drag;
    POINT anchor;
    Source *source;
    ULONG_PTR gdiplus;
    CRITICAL_SECTION lock;
    HANDLE wake,worker;
    BOOL stopping,busy;
    Tile active, jobs[CACHE_COUNT],cache[CACHE_COUNT];
    int pending;
} Canvas;

static Canvas *canvas(HWND window){return (Canvas*)GetWindowLongPtrW(window,GWLP_USERDATA);}
static void release(Source *s){if(s && !InterlockedDecrement(&s->refs)){map_vector_close(s->vector);free(s);}}
static void color_fill(HDC dc,RECT rect,COLORREF col){SetDCBrushColor(dc,col);FillRect(dc,&rect,GetStockObject(DC_BRUSH));}
static GpImage *tile_image(const wchar_t *path){
    WIN32_FILE_ATTRIBUTE_DATA f;
    if(!GetFileAttributesExW(path,GetFileExInfoStandard,&f) || f.nFileSizeHigh || f.nFileSizeLow>2*1024*1024 || f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) return NULL;
    GpImage *im=NULL; UINT w=0,h=0;
    if(GdipLoadImageFromFile(path,&im)!=Ok || !im) return NULL;
    if(GdipGetImageWidth(im,&w)!=Ok || GdipGetImageHeight(im,&h)!=Ok || w!=256 || h!=256){GdipDisposeImage(im);return NULL;}
    return im;
}
BOOL map_valid_tile(const wchar_t *path) {
    GpImage *im=tile_image(path); if(!im) return FALSE;
    HBITMAP decoded=NULL;GpStatus result=GdipCreateHBITMAPFromBitmap((GpBitmap *)im,&decoded,0);
    if(decoded)DeleteObject(decoded);
    GdipDisposeImage(im);return result==Ok;
}
static HBITMAP draw_tile(Source *s,Tile t) {
    BITMAPINFO info={.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=256,.biHeight=-256,.biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
    HDC dc=CreateCompatibleDC(NULL);void *pixels=NULL;
    HBITMAP bmp=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);
    if(!dc || !bmp){if(dc)DeleteDC(dc);if(bmp)DeleteObject(bmp);return NULL;}
    HGDIOBJ old=SelectObject(dc,bmp); RECT box={0,0,256,256};color_fill(dc,box,RGB(241,240,231));
    if(s->vector)map_vector_tile(s->vector,dc,t.z,t.x,t.y);
    else {
        wchar_t path[MAP_PATH+80];GpImage *im=NULL;
        const wchar_t *exts[]={L"png",L"jpg",L"jpeg"};
        for(int i=0;i<3 && !im;i++){
            StringCchPrintfW(path,ARRAYSIZE(path),L"%ls\\%d\\%d\\%d.%ls",s->path,t.z,t.x,t.y,exts[i]);
            im=tile_image(path);
        }
        if(im){
            GpGraphics *g=NULL;
            if(GdipCreateFromHDC(dc,&g)==Ok){GdipDrawImageRectI(g,im,0,0,256,256);GdipDeleteGraphics(g);}
            GdipDisposeImage(im);
        }else {
            FrameRect(dc,&box,GetSysColorBrush(COLOR_3DFACE));
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(132,136,130));SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(dc,L"Keine Kachel",-1,&box,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
    }
    GdiFlush();SelectObject(dc,old);DeleteDC(dc);return bmp;
}
static BOOL same(Tile a,Tile b){return a.x==b.x && a.y==b.y && a.z==b.z;}
static DWORD WINAPI render_worker(void *arg){
    Canvas *c=arg;
    for(;;){
        WaitForSingleObject(c->wake,INFINITE);
        EnterCriticalSection(&c->lock);
        if(c->stopping){LeaveCriticalSection(&c->lock);break;}
        if(!c->source || !c->pending){ResetEvent(c->wake);LeaveCriticalSection(&c->lock);continue;}
        Tile t=c->jobs[--c->pending];Source *s=c->source;InterlockedIncrement(&s->refs);c->busy=TRUE;c->active=t;
        LeaveCriticalSection(&c->lock);
        HBITMAP bitmap=draw_tile(s,t);
        EnterCriticalSection(&c->lock);
        if(c->source==s && !c->stopping){
            int slot=0;
            for(int i=0;i<CACHE_COUNT;i++){
                if(!c->cache[i].valid){slot=i;break;}
                if(c->cache[i].used<c->cache[slot].used)slot=i;
            }
            if(c->cache[slot].bitmap)DeleteObject(c->cache[slot].bitmap);
            t.valid=TRUE;t.used=GetTickCount64();t.bitmap=bitmap;c->cache[slot]=t;bitmap=NULL;
            PostMessageW(c->window,TILE_READY,0,0);
        }
        c->busy=FALSE;
        LeaveCriticalSection(&c->lock);
        if(bitmap) DeleteObject(bitmap);
        release(s);
    }
    return 0;
}
static void clear_cache(Canvas *c) {
    c->pending=0;
    for(int i=0;i<CACHE_COUNT;i++){if(c->cache[i].bitmap)DeleteObject(c->cache[i].bitmap);c->cache[i]=(Tile){0};}
}
static void source(Canvas *c,Source *s,double lat,double lon,int z) {
    EnterCriticalSection(&c->lock);Source *old=c->source;c->source=s;clear_cache(c);LeaveCriticalSection(&c->lock);
    release(old);
    c->x=map_x(lon);c->y=map_y(lat);c->zoom=z;
    HKEY key;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,MAP_REG,0,NULL,0,KEY_WRITE,NULL,&key,NULL)==ERROR_SUCCESS){
        RegSetValueExW(key,L"Path",0,REG_SZ,(BYTE*)s->path,(DWORD)((wcslen(s->path)+1)*sizeof(wchar_t)));
        RegSetValueExW(key,L"Credit",0,REG_SZ,(BYTE*)s->credit,(DWORD)((wcslen(s->credit)+1)*sizeof(wchar_t)));
        double view[]={lat,lon,(double)z}; RegSetValueExW(key,L"View",0,REG_BINARY,(BYTE*)view,sizeof view);
        RegCloseKey(key);
    }
    InvalidateRect(c->window,NULL,FALSE);
}
BOOL map_use_file(HWND window,const wchar_t *path){
    Canvas *c=canvas(window);if(!c)return FALSE;
    Source *s=calloc(1,sizeof *s);if(!s)return FALSE;
    s->refs=1;s->vector=map_vector_open(path);
    if(!s->vector || FAILED(StringCchCopyW(s->path,MAP_PATH,path))){release(s);return FALSE;}
    StringCchCopyW(s->credit,ARRAYSIZE(s->credit),MAP_CREDIT);
    double lat,lon;int z;map_vector_home(s->vector,&lat,&lon,&z);source(c,s,lat,lon,z);return TRUE;
}
BOOL map_use_directory(HWND window,const wchar_t *path,const wchar_t *credit,double lat,double lon,int z){
    Canvas *c=canvas(window);
    DWORD attrs=GetFileAttributesW(path);
    if(!c || attrs==INVALID_FILE_ATTRIBUTES || !(attrs&FILE_ATTRIBUTE_DIRECTORY))return FALSE;
    Source *s=calloc(1,sizeof *s);if(!s)return FALSE;
    s->refs=1;
    if(FAILED(StringCchCopyW(s->path,MAP_PATH,path))){release(s);return FALSE;}
    StringCchCopyW(s->credit,ARRAYSIZE(s->credit),credit);
    source(c,s,lat,lon,(int)fmax(0,fmin(20,z)));return TRUE;
}
void map_current(HWND window,wchar_t *path,size_t cap,double *lat,double *lon){
    Canvas *c=canvas(window);if(!c)return;
    StringCchCopyW(path,cap,c->source?c->source->path:L"");*lat=map_lat(c->y);*lon=map_lon(c->x);
}
static void render(Canvas *c,HDC dc,RECT r){
    color_fill(dc,r,RGB(240,242,237));
    double scale=ldexp(256.,c->zoom), left=c->x*scale-r.right/2.,top=c->y*scale-r.bottom/2.;
    int tiles=1<<c->zoom;
    HDC memory=CreateCompatibleDC(dc);
    EnterCriticalSection(&c->lock);c->pending=0;
    for(int y=(int)floor(top/256.);y<=(int)floor((top+r.bottom)/256.);y++){
        if(y<0 || y>=tiles) continue;
        for(int x=(int)floor(left/256.);x<=(int)floor((left+r.right)/256.);x++){
            int sx=(int)round(x*256.-left),sy=(int)round(y*256.-top);
            Tile t={.x=(x%tiles+tiles)%tiles,.y=y,.z=c->zoom};BOOL found=FALSE;
            if(c->source){
                for(int i=0;i<CACHE_COUNT;i++)if(c->cache[i].valid && same(c->cache[i],t)){
                    c->cache[i].used=GetTickCount64();
                    if(c->cache[i].bitmap){HGDIOBJ old=SelectObject(memory,c->cache[i].bitmap);BitBlt(dc,sx,sy,256,256,memory,0,0,SRCCOPY);SelectObject(memory,old);}
                    found=TRUE;break;
                }
                if(!found && c->pending<CACHE_COUNT && !(c->busy && same(t,c->active))) {
                    BOOL queued=FALSE;for(int i=0;i<c->pending;i++)if(same(t,c->jobs[i])){queued=TRUE;break;}
                    if(!queued)c->jobs[c->pending++]=t;
                }
            }
            if(!found){RECT tile={sx,sy,sx+256,sy+256};FrameRect(dc,&tile,GetSysColorBrush(COLOR_3DFACE));}
        }
    }
    if(c->pending)SetEvent(c->wake);
    LeaveCriticalSection(&c->lock);DeleteDC(memory);
    SetBkMode(dc,TRANSPARENT); SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));
    if(!c->source){SetTextColor(dc,RGB(102,113,108));RECT hint={8,8,r.right-8,r.bottom};DrawTextW(dc,L"Offlinekarte über „Karten“ öffnen",-1,&hint,DT_LEFT|DT_TOP|DT_SINGLELINE);}
    HPEN pen=CreatePen(PS_SOLID,3,RGB(4,109,108));HGDIOBJ old=SelectObject(dc,pen);
    SelectObject(dc,GetStockObject(DC_BRUSH));SetDCBrushColor(dc,RGB(4,109,108));
    for(int pass=0;pass<2;pass++){
        for(size_t i=0;i<c->count;i++){
            double dx=map_x(c->points[i].lon)-c->x;dx-=round(dx);
            double px=r.right/2.+scale*dx, py=r.bottom/2.+scale*(map_y(c->points[i].lat)-c->y);
            int x=(int)round(px),y=(int)round(py);
            if(!pass){if(i && c->lines)LineTo(dc,x,y);MoveToEx(dc,x,y,NULL);continue;}
            if(x < -16 || y < -16 || x>r.right+16 || y>r.bottom+16)continue;
            BOOL chosen=i==(size_t)c->selected;int radius=chosen?7:5;
            SetDCBrushColor(dc,chosen?RGB(248,181,63):RGB(255,255,255));Ellipse(dc,x-radius,y-radius,x+radius+1,y+radius+1);
            if(c->labels){
                wchar_t number[32];StringCchPrintfW(number,32,L"%zu",i+1);
                const wchar_t *label=c->points[i].name && c->points[i].name[0]?c->points[i].name:number;
                RECT box={x+8,y-9,r.right-2,y+11};
                SetBkMode(dc,OPAQUE);SetBkColor(dc,RGB(250,249,244));SetTextColor(dc,RGB(30,70,73));
                DrawTextW(dc,label,-1,&box,DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);SetBkMode(dc,TRANSPARENT);
            }
        }
    }
    SelectObject(dc,old);DeleteObject(pen);
    wchar_t status[720];
    StringCchPrintfW(status,ARRAYSIZE(status),L"%.5f°, %.5f°   ·   Zoom %d   %ls",map_lat(c->y),map_lon(c->x),c->zoom,c->source?c->source->credit:L"");
    RECT strip={0,r.bottom-20,r.right,r.bottom};color_fill(dc,strip,RGB(251,251,248));strip.left+=5;
    SetTextColor(dc,RGB(43,58,54));DrawTextW(dc,status,-1,&strip,DT_SINGLELINE|DT_NOPREFIX|DT_VCENTER|DT_END_ELLIPSIS);
}
static void clear_points(Canvas *c){
    for(size_t i=0;i<c->count;i++)free((void*)c->points[i].name);
    free(c->points);c->points=NULL;c->count=0;
}
void map_points(HWND window,const MapPoint *p,size_t n,int selected){
    Canvas *c=canvas(window);if(!c)return;
    MapPoint *copy=calloc(n,sizeof *copy);if(n && !copy)return;
    for(size_t i=0;i<n;i++){copy[i]=p[i];copy[i].name=p[i].name?_wcsdup(p[i].name):NULL;}
    clear_points(c);c->points=copy;c->count=n;c->selected=selected;InvalidateRect(window,NULL,FALSE);
}
void map_center(HWND window,double lat,double lon){
    Canvas *c=canvas(window);if(!c || !isfinite(lat) || !isfinite(lon))return;
    c->x=map_x(lon);c->y=map_y(lat);InvalidateRect(window,NULL,FALSE);
}
void map_fit(HWND window){
    Canvas *c=canvas(window);if(!c || !c->count)return;
    double loX=DBL_MAX,hiX=-DBL_MAX,loY=DBL_MAX,hiY=-DBL_MAX,origin=map_x(c->points[0].lon);
    for(size_t i=0;i<c->count;i++){
        double dx=map_x(c->points[i].lon)-origin,x=origin+dx-round(dx),y=map_y(c->points[i].lat);
        loX=fmin(loX,x);hiX=fmax(hiX,x);loY=fmin(loY,y);hiY=fmax(hiY,y);
    }
    RECT r;GetClientRect(window,&r);
    double xScale=fmax(40.,r.right-80.)/fmax(1e-8,hiX-loX),yScale=fmax(40.,r.bottom-65.)/fmax(1e-8,hiY-loY);
    c->zoom=(int)fmax(0.,fmin(16.,floor(log2(fmin(xScale,yScale)/256.))));c->x=(loX+hiX)/2.;c->x-=floor(c->x);c->y=(loY+hiY)/2.;
    InvalidateRect(window,NULL,FALSE);
}
void map_zoom(HWND window,int delta){Canvas *c=canvas(window);if(c){c->zoom=(int)fmax(0.,fmin(20.,c->zoom+delta));InvalidateRect(window,NULL,FALSE);}}
void map_overlay(HWND window,BOOL lines,BOOL labels){Canvas *c=canvas(window);if(c){c->lines=lines;c->labels=labels;InvalidateRect(window,NULL,FALSE);}}
void map_manage(HWND window){if(canvas(window)) map_dialog(window);}
static void pan(Canvas *c,int dx,int dy) {
    double scale=ldexp(256.,c->zoom);c->x-=dx/scale;c->x-=floor(c->x);c->y=fmax(0.,fmin(1.,c->y-dy/scale));
    InvalidateRect(c->window,NULL,FALSE);
}
static LRESULT CALLBACK window_proc(HWND window,UINT msg,WPARAM wp,LPARAM lp){
    Canvas *c=canvas(window);
    switch(msg){
    case WM_NCCREATE: c=((CREATESTRUCTW*)lp)->lpCreateParams;c->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)c);return TRUE;
    case WM_ERASEBKGND:return 1;
    case TILE_READY:InvalidateRect(window,NULL,FALSE);return 0;
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC dc=BeginPaint(window,&ps);RECT r;GetClientRect(window,&r);
        HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,r.right,r.bottom);
        if(bmp && mem){HGDIOBJ old=SelectObject(mem,bmp);render(c,mem,r);BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);SelectObject(mem,old);}
        DeleteObject(bmp);DeleteDC(mem);EndPaint(window,&ps);return 0;
    }
    case WM_LBUTTONDOWN:SetFocus(window);SetCapture(window);c->drag=TRUE;c->anchor=(POINT){GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};return 0;
    case WM_LBUTTONUP:if(c->drag)ReleaseCapture();c->drag=FALSE;return 0;
    case WM_CAPTURECHANGED:c->drag=FALSE;return 0;
    case WM_MOUSEMOVE:if(c->drag){POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};pan(c,pt.x-c->anchor.x,pt.y-c->anchor.y);c->anchor=pt;}return 0;
    case WM_LBUTTONDBLCLK:if(c->pick){
        RECT r;GetClientRect(window,&r);double d=ldexp(256.,c->zoom);
        double y=c->y+(GET_Y_LPARAM(lp)-r.bottom/2.)/d, x=c->x+(GET_X_LPARAM(lp)-r.right/2.)/d;
        c->pick(map_lat(fmax(0.,fmin(1.,y))),map_lon(x),c->context);
    }return 0;
    case WM_MOUSEWHEEL:{
        RECT r;GetClientRect(window,&r);POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(window,&pt);
        int step=GET_WHEEL_DELTA_WPARAM(wp)>0?1:-1;double before=ldexp(256.,c->zoom);
        map_zoom(window,step);double after=ldexp(256.,c->zoom);
        c->x+=(pt.x-r.right/2.)*(1./before-1./after);c->x-=floor(c->x);
        c->y=fmax(0.,fmin(1.,c->y+(pt.y-r.bottom/2.)*(1./before-1./after)));return 0;
    }
    case WM_KEYDOWN:
        switch(wp){case VK_LEFT:pan(c,70,0);break;case VK_RIGHT:pan(c,-70,0);break;case VK_UP:pan(c,0,70);break;case VK_DOWN:pan(c,0,-70);break;case VK_ADD:case VK_OEM_PLUS:map_zoom(window,1);break;case VK_SUBTRACT:case VK_OEM_MINUS:map_zoom(window,-1);break;case VK_HOME:map_fit(window);break;}return 0;
    case WM_GETDLGCODE:return DLGC_WANTARROWS;
    case WM_DESTROY:
        EnterCriticalSection(&c->lock);c->stopping=TRUE;SetEvent(c->wake);LeaveCriticalSection(&c->lock);
        if(c->worker){WaitForSingleObject(c->worker,INFINITE);CloseHandle(c->worker);}
        CloseHandle(c->wake);clear_cache(c);release(c->source);DeleteCriticalSection(&c->lock);
        clear_points(c);if(c->gdiplus)GdiplusShutdown(c->gdiplus);SetWindowLongPtrW(window,GWLP_USERDATA,0);free(c);return 0;
    }
    return DefWindowProcW(window,msg,wp,lp);
}
HWND map_create(HWND parent,int id,MapPick pick,void *context) {
    HINSTANCE instance=GetModuleHandleW(NULL);
    WNDCLASSW wc={.style=CS_DBLCLKS|CS_HREDRAW|CS_VREDRAW,.lpfnWndProc=window_proc,.hInstance=instance,.hCursor=LoadCursorW(NULL,IDC_CROSS),.lpszClassName=L"GeoVoiceMapCanvas"};
    RegisterClassW(&wc); Canvas *c=calloc(1,sizeof *c); if(!c)return NULL;
    c->pick=pick;c->context=context;c->x=map_x(16.3738);c->y=map_y(48.2082);c->zoom=14;c->lines=TRUE;c->labels=TRUE;
    InitializeCriticalSection(&c->lock);c->wake=CreateEventW(NULL,TRUE,FALSE,NULL);
    GdiplusStartupInput input={.GdiplusVersion=1};
    if(!c->wake || GdiplusStartup(&c->gdiplus,&input,NULL)!=Ok){if(c->wake)CloseHandle(c->wake);DeleteCriticalSection(&c->lock);free(c);return NULL;}
    HWND window=CreateWindowExW(WS_EX_CLIENTEDGE,wc.lpszClassName,L"Kartenansicht",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,100,100,parent,(HMENU)(INT_PTR)id,instance,c);
    if(!window){GdiplusShutdown(c->gdiplus);CloseHandle(c->wake);DeleteCriticalSection(&c->lock);free(c);return NULL;}
    c->worker=CreateThread(NULL,0,render_worker,c,0,NULL);
    if(!c->worker){DestroyWindow(window);return NULL;}
    wchar_t path[MAP_PATH]={0},credit[512]={0};DWORD bytes=sizeof path;
    if(RegGetValueW(HKEY_CURRENT_USER,MAP_REG,L"Path",RRF_RT_REG_SZ,NULL,path,&bytes)==ERROR_SUCCESS){
        DWORD attrs=GetFileAttributesW(path);
        if(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_DIRECTORY)){
            bytes=sizeof credit;RegGetValueW(HKEY_CURRENT_USER,MAP_REG,L"Credit",RRF_RT_REG_SZ,NULL,credit,&bytes);
            double view[]={48.2082,16.3738,14.};bytes=sizeof view;
            RegGetValueW(HKEY_CURRENT_USER,MAP_REG,L"View",RRF_RT_REG_BINARY,NULL,view,&bytes);
            if(isfinite(view[0]) && fabs(view[0])<=90 && isfinite(view[1]) && fabs(view[1])<=180 && isfinite(view[2]) && view[2]>=0 && view[2]<=20)map_use_directory(window,path,credit,view[0],view[1],(int)view[2]);
        }else if(attrs!=INVALID_FILE_ATTRIBUTES) map_use_file(window,path);
    }
    return window;
}
