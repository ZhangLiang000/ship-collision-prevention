/******************************************************************************
 * $Id:   $
 *
 * Project:  OpenCPN
 * Purpose:  RadarView Object
 * Author:   Johan van der Sman
 *
 ***************************************************************************
 *   Copyright (C) 2015 Johan van der Sman                                 *
 *   johan.sman@gmail.com                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.         *
 ***************************************************************************
 *
 */
#include "wx/wx.h"
#include <wx/debug.h>
#include <wx/fileconf.h>
#include <wx/socket.h>
#include <math.h>
#include "aisradar_pi.h"
#include "Canvas.h"
// #include "kalman/FusionEKF.h"
#include <map>
#include <list>
#include <algorithm>
#include <thread>

#define  min(a,b)  ( (a>b)? b : a )
#define  max(a,b)  ( (a>b)? a : b )

#if wxUSE_IPV6
    typedef wxIPV6address IPaddress;
#else
    typedef wxIPV4address IPaddress;
#endif

using namespace std;

extern double           gLat, gLon;
extern double           gCog, gSog;
extern double g_n_ownship_length_meters;
extern double g_n_ownship_beam_meters;


static const double    RangeData[9] = {
    0.25, 0.5, 1, 2, 4, 8, 12, 16, 32
};

enum    Ids { cbRangeId = 10001,
                cbNorthUpId,
                cbBearingLineId,
                btShowAisList,
                tmRefreshId,
                plCanvasId,

                connectOptionLinkId,        // option button
                tmTTSId,                    // TTS timer
                soundPlayId,                // open button
                // id for socket
                SOCKET_ID,
                SERVER_ID
};

static const int RESTART  = -1;
static const int BASE_STATION = 3;

double a1 = 6;    //AIS距离标准差
double a2 = 0.4;   //AIS方位标准差
double a3 = 0.161;    //AIS航速标准差
double a4 = 0.637;    //AIS航向标准差
double r1 = 20;     //Radar距离标准差
double r2 = 0.8;      //Radar方位标准差
double r3 = 0.148;    //Radar航速标准差
double r4 = 0.743;     //Radar航向标准差

//计算融合的权重系数
double a11 = (r1*r1) / (a1*a1 + r1*r1);
double a12 = (a1*a1) / (a1*a1 + r1*r1);
double a21 = (r2*r2) / (a2*a2 + r2*r2);
double a22 = (a2*a2) / (a2*a2 + r2*r2);
double a31 = (r3*r3) / (a3*a3 + r3*r3);
double a32 = (a3*a3) / (a3*a3 + r3*r3);
double a41 = (r4*r4) / (a4*a4 + r4*r4);
double a42 = (a4*a4) / (a4*a4 + r4*r4);

#include <sys/wait.h> // for WEXITSTATUS & friends

static int do_play(const char* cmd)
{
    char buff[1024];

    snprintf(buff, sizeof( buff ), "espeak-ng  '%s' -s 20 -p 15 -v zh", cmd);

    int status = system(buff);
    if (status == -1) {
        wxLogWarning("Cannot fork process running %s", buff);
        return -1;
    }
    if (WIFEXITED(status)) {
        status = WEXITSTATUS(status);
        if (status != 0) {
            wxLogWarning("Exit code %d from command %s",
                status, buff);
        }
    } else {
        wxLogWarning("Strange return code %d (0x%x) running %s",
                     status, status, buff);
    }
    return status;
}

void executeCMD(const char *cmd, char *result)   
{   
    char buf_ps[1024];   
    char ps[1024]={0};   
    FILE *ptr;   
    strcpy(ps, cmd);   
    if((ptr=popen(ps, "r"))!=NULL)   
    {   
        while(fgets(buf_ps, 1024, ptr)!=NULL)   
        {   
           strcat(result, buf_ps);   
           if(strlen(result)>1024)   
               break;   
        }   
        pclose(ptr);   
        ptr = NULL;   
    }   
    else  
    {   
        printf("popen %s error\n", ps);   
    }   
}

//---------------------------------------------------------------------------------------
//          Radar Dialog Implementation
//---------------------------------------------------------------------------------------
IMPLEMENT_CLASS ( RadarFrame, wxFrame )

BEGIN_EVENT_TABLE ( RadarFrame, wxFrame )

    EVT_CLOSE    ( RadarFrame::OnClose )
    EVT_MOVE     ( RadarFrame::OnMove )
    EVT_SIZE     ( RadarFrame::OnSize )
//    EVT_PAINT    ( RadarFrame::paintEvent)
    EVT_COMBOBOX ( cbRangeId, RadarFrame::OnRange)
    EVT_CHECKBOX ( cbNorthUpId, RadarFrame::OnNorthUp )
    EVT_CHECKBOX ( cbBearingLineId, RadarFrame::OnBearingLine )
    EVT_TIMER    ( tmRefreshId, RadarFrame::OnTimer )
    // EVT_TIMER    ( tmTTSId, RadarFrame::TTSPlaySoundTimer )
    // EVT_BUTTON   ( soundPlayId, RadarFrame::TTSPlaySound )
    // EVT_BUTTON   ( connectOptionLinkId, RadarFrame::SetConnectOption )
    // EVT_SOCKET   ( SOCKET_ID,     RadarFrame::OnSocketEvent) 
    EVT_SOCKET(SERVER_ID,  RadarFrame::OnServerEvent)
    EVT_SOCKET(SOCKET_ID,  RadarFrame::OnSocketEvent)
END_EVENT_TABLE()

RadarFrame::RadarFrame() 
: pParent(0), 
    pPlugIn(0), 
    m_Timer(0), 
    m_pCanvas(0), 
    m_pNorthUp(0), 
    m_pRange(0),
    m_pBearingLine(0), 
    m_BgColour(),
    m_Ebl(0.0),  
    m_Range(0), 
    m_pViewState(0)
{
    Init();
}

RadarFrame::~RadarFrame( ) {
}


void RadarFrame::Init() {
    GetGlobalColor(_T("DILG1"), &m_BgColour);
    SetBackgroundColour(m_BgColour);
}


bool RadarFrame::Create ( wxWindow *parent, aisradar_pi *ppi, wxWindowID id,
                              const wxString& caption, 
                              const wxPoint& pos, const wxSize& size )
{
    pParent = parent;
    pPlugIn = ppi;
    long wstyle = wxDEFAULT_FRAME_STYLE;
    m_pViewState = new ViewState(pos, size);
    if ( !wxFrame::Create ( parent, id, caption, pos, m_pViewState->GetWindowSize(), wstyle ) ) {
        return false;
    }

    // Add panel to contents of frame
    wxPanel *panel = new wxPanel(this, plCanvasId );
    panel->SetAutoLayout(true);
    wxBoxSizer *vbox = new wxBoxSizer(wxVERTICAL);
    panel->SetSizer(vbox);

    // Add Canvas panel to draw upon
    // Use the stored size provided by the pi
    // Create a square box based on the smallest side
    wxBoxSizer *canvas = new wxBoxSizer(wxHORIZONTAL);
    m_pCanvas = new Canvas(panel, this, wxID_ANY, pos, m_pViewState->GetCanvasSize());
    m_pCanvas->SetBackgroundStyle(wxBG_STYLE_CUSTOM);
    wxBoxSizer *cbox = new wxBoxSizer(wxVERTICAL);
    cbox->FitInside(m_pCanvas);
    canvas->Add(m_pCanvas, 1, wxEXPAND);
    vbox->Add(canvas, 1, wxALL | wxEXPAND, 5);
  
    // Add controls
    wxStaticBox    *sb=new wxStaticBox(panel,wxID_ANY, _("Options"));
    wxStaticBoxSizer *controls = new wxStaticBoxSizer(sb, wxHORIZONTAL);
    wxStaticText *st1 = new wxStaticText(panel,wxID_ANY,_("Range"));
    controls->Add(st1,0,wxRIGHT,5);
    m_pRange = new wxComboBox(panel, cbRangeId, wxT(""));
    m_pRange->Append(wxT("0.25"));
    m_pRange->Append(wxT("0.5") );
    m_pRange->Append(wxT("1")   );
    m_pRange->Append(wxT("2")   );
    m_pRange->Append(wxT("4")   );
    m_pRange->Append(wxT("8")   );
    m_pRange->Append(wxT("12")  );
    m_pRange->Append(wxT("16")  );
    m_pRange->Append(wxT("32")  );
    m_pRange->SetSelection(pPlugIn->GetRadarRange());
    controls->Add(m_pRange);

    wxStaticText *st2 = new wxStaticText(panel,wxID_ANY,_("Nautical Miles"));
    controls->Add(st2,0,wxRIGHT|wxLEFT,5);

    m_pNorthUp = new wxCheckBox(panel, cbNorthUpId, _("North Up"));
    m_pNorthUp->SetValue(pPlugIn->GetRadarNorthUp());
    controls->Add(m_pNorthUp, 0, wxLEFT, 10);

    m_pBearingLine = new wxCheckBox(panel, cbBearingLineId, _("EBL"));
    m_pBearingLine->SetValue(false);
    controls->Add(m_pBearingLine, 0, wxLEFT, 10);
    
    m_pShowList = new wxButton(panel, btShowAisList, _("ShowAisList"));
    controls->Add(m_pShowList, 0, wxLEFT, 10);

    vbox->Add(controls, 0, wxEXPAND | wxALL, 5);

    //加一个语音播报窗口
    wxStaticBoxSizer* sbSizer1;
	sbSizer1 = new wxStaticBoxSizer( new wxStaticBox( panel, wxID_ANY, wxT("Instruction:") ), wxHORIZONTAL );
    sbSizer1->SetMinSize( wxSize( -1,150 ) ); 
    
    wxBoxSizer* bSizer1;
	bSizer1 = new wxBoxSizer( wxHORIZONTAL );

    wxStaticText *m_warningInstructionLabel = new wxStaticText(panel, wxID_ANY, wxT("Advices:"), wxDefaultPosition, wxSize(-1,-1), 0);
    m_warningInstructionLabel->Wrap(-1);
    sbSizer1->Add( m_warningInstructionLabel, 0, wxALIGN_CENTER, 5 );
    m_textCtrl1 = new wxTextCtrl( panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize( 300,50 ), wxTE_MULTILINE | wxTE_READONLY);
    sbSizer1->Add( m_textCtrl1, 1, wxALIGN_CENTER|wxALL|wxEXPAND, 5 );
    // delete wxLog::SetActiveTarget(new wxLogTextCtrl(m_textCtrl1));

    wxBoxSizer *m_buttonBox;
    m_buttonBox = new wxBoxSizer( wxVERTICAL );
    
    m_ConnectOptionButton = new wxButton( panel, connectOptionLinkId, wxT("Option"), wxPoint( -1,-1 ), wxDefaultSize, 0 );
    m_buttonBox->Add( m_ConnectOptionButton, 0, wxALIGN_CENTER_HORIZONTAL|wxALIGN_CENTER_VERTICAL|wxTOP|wxBOTTOM, 5 );
    m_soundButton = new wxButton( panel, soundPlayId, wxT("Open"), wxPoint( -1,-1 ), wxDefaultSize, 0 );
    m_buttonBox->Add( m_soundButton, 0, wxALIGN_CENTER_HORIZONTAL|wxALIGN_CENTER_VERTICAL|wxTOP|wxBOTTOM, 5  );
    m_soundButton->Disable();
    sbSizer1->Add( m_buttonBox, 0, wxALIGN_CENTER, 5 );
    
    vbox->Add(sbSizer1, 0, wxEXPAND | wxALL, 5);
    
    // Add timer
    m_Timer = new wxTimer(this, tmRefreshId);
    m_Timer->Start(2000);

    m_Timer_TTS = new wxTimer(this, tmTTSId);
    m_Timer_TTS->Start(6000);

    vbox->MyFit(this);

    // // Create the socket
    // m_sock = new wxSocketClient();

    // // Setup the event handler and subscribe to most events
    // m_sock->SetEventHandler(*this, SOCKET_ID);
    // m_sock->SetNotify(wxSOCKET_CONNECTION_FLAG |
    //                     wxSOCKET_INPUT_FLAG |
    //                     wxSOCKET_LOST_FLAG);
    // m_sock->Notify(true);
    // m_busy = false;

    // Create the address - defaults to localhost:0 initially
    IPaddress addr;
    addr.Service(20002);

    wxLogMessage("Creating server at %s:%u", addr.IPAddress(), addr.Service());

    // Create the socket
    m_server = new wxSocketServer(addr);

    // We use IsOk() here to see if the server is really listening
    if (! m_server->IsOk())
    {
        wxLogMessage("Could not listen at the specified port !");
        return true;
    }

    IPaddress addrReal;
    if ( !m_server->GetLocal(addrReal) )
    {
        wxLogMessage("ERROR: couldn't get the address we bound to");
    }
    else
    {
        wxLogMessage("Server listening at %s:%u",
                    addrReal.IPAddress(), addrReal.Service());
    }

    // Setup the event handler and subscribe to connection events
    m_server->SetEventHandler(*this, SERVER_ID);
    m_server->SetNotify(wxSOCKET_CONNECTION_FLAG);
    m_server->Notify(true);

    m_busy = false;
    m_numClients = 0;
#if wxUSE_STATUSBAR
    // Status bar
    CreateStatusBar(2);
#endif // wxUSE_STATUSBAR

    UpdateStatusBar();

    return true;
}


void RadarFrame::SetColourScheme(PI_ColorScheme cs) {
      GetGlobalColor(_T("DILG1"), &m_BgColour);
      SetBackgroundColour(m_BgColour);
      this->Refresh();
}


void RadarFrame::OnClose ( wxCloseEvent& event ) {
    // Stop timer if still running
    m_Timer->Stop();
    m_Timer_TTS->Stop();
    delete m_Timer;
    delete m_Timer_TTS;
    // delete m_sock;
    
    // Save window size
    pPlugIn->SetRadarFrameX(m_pViewState->GetPos().x);
    pPlugIn->SetRadarFrameY(m_pViewState->GetPos().y);
    pPlugIn->SetRadarFrameSizeX(m_pViewState->GetSize().GetWidth());
    pPlugIn->SetRadarFrameSizeY(m_pViewState->GetSize().GetHeight());

    // Cleanup
    RequestRefresh(pParent);
    Destroy();
    pPlugIn->OnRadarFrameClose();
}


void RadarFrame::OnRange ( wxCommandEvent& event ) {
    pPlugIn->SetRadarRange(m_pRange->GetSelection());
    this->Refresh();
}



void RadarFrame::OnNorthUp ( wxCommandEvent& event ) {
    pPlugIn->SetRadarNorthUp(m_pNorthUp->GetValue());
    if (m_pNorthUp->GetValue()) {
        m_Ebl += pPlugIn->GetCog();
    } else {
        m_Ebl -= pPlugIn->GetCog();
    }
    this->Refresh();
}


void RadarFrame::OnTimer( wxTimerEvent& event ) {
    this->Refresh();
}


void RadarFrame::OnBearingLine( wxCommandEvent& event ) {
    this->Refresh();
}


void RadarFrame::OnLeftMouse(const wxPoint &curpos) {
    if (m_pBearingLine->GetValue()) {
        int width      = max(m_pCanvas->GetSize().GetWidth(), (MIN_RADIUS)*2 );
        int height     = max(m_pCanvas->GetSize().GetHeight(),(MIN_RADIUS)*2 );
        wxPoint center(width/2, height/2);
        int dx = curpos.x - center.x;
        int dy = center.y - curpos.y;    // top of screen y=0
        double tmpradius = sqrt((double)(dx*dx)+(double)(dy*dy));
        double angle= dy/tmpradius;
        m_Ebl = asin(angle)*(double)((double)180./(double)3.141592653589);
        if ( dx >= 0 ) {
            m_Ebl = 90 - m_Ebl;
        } else {
            m_Ebl = 360 - (90 - m_Ebl);
        }
        this->Refresh();
    }
}


void RadarFrame::OnMove ( wxMoveEvent& event ) {
    wxPoint p = event.GetPosition();

    // Save window position
    m_pViewState->SetPos(wxPoint(p.x, p.y));
    event.Skip();
}


void RadarFrame::OnSize ( wxSizeEvent& event ) {
    event.Skip();
    if( m_pCanvas )
    {
        wxClientDC dc(m_pCanvas);
        m_pViewState->SetCanvasSize(GetSize());
        m_pViewState->SetWindowSize(GetSize());
        render(dc);
    }
}


void RadarFrame::paintEvent(wxPaintEvent & event) {
    wxAutoBufferedPaintDC   dc(m_pCanvas);
    render(dc);
    event.Skip();
}
#if 0 // Client
/**
 * @name: 根据IP地址和端口号建立socket连接，通过按键m_ConnectOptionButton事件触发
 * @msg:  Open session - 打开连接  Close session - 关闭连接
 * @param {type} 
 * @return: 
 */
void RadarFrame::SetConnectOption( wxCommandEvent& event ){
    if (!m_sock->IsConnected() && m_ConnectOptionButton->GetLabel() == "Open session" ) {
        OpenConnection(wxSockAddress::IPV4);
        // Wait until the request completes or until we decide to give up
        bool waitmore = true;
        while ( !m_sock->WaitOnConnect(-1, 3000) && waitmore )
        {
            
        }
        if (m_sock->IsConnected()) {
            m_ConnectOptionButton->SetLabel("Close session");
            m_soundButton->Enable();
            UpdateStatusBar();
        }
        
    }
    else if ( m_sock->IsConnected() && m_ConnectOptionButton->GetLabel() == "Close session" ) {
        m_sock->Close();
        m_ConnectOptionButton->SetLabel("Open session");
        UpdateStatusBar();
        m_soundButton->SetLabel("Open");
        m_soundButton->Disable();
    }
    else{
        SetStatusText("State : 0", 1);
    }
}

/**
 * @name: 建立连接
 * @msg: 
 * @param {type} 
 * @return: 
 */
void RadarFrame::OpenConnection(wxSockAddress::Family family)
{
    wxUnusedVar(family); // unused in !wxUSE_IPV6 case

    wxIPaddress * addr;
    wxIPV4address addr4;
    addr = &addr4;
#if wxUSE_IPV6
    wxIPV6address addr6;
    if ( family == wxSockAddress::IPV6 )
        addr = &addr6;
    else
#endif
    // Ask user for server address
    wxString hostname = wxGetTextFromUser(
        _("Enter the address of the wxSocket demo server:"),
        _("Connect ..."),
        _("localhost"));
    if ( hostname.empty() )
        return;

    addr->Hostname(hostname);
    addr->Service(3000);

    // we connect asynchronously and will get a wxSOCKET_CONNECTION event when
    // the connection is really established
    //
    // if you want to make sure that connection is established right here you
    // could call WaitOnConnect(timeout) instead
    // wxLogMessage("Trying to connect to %s:%d", hostname, addr->Service());

    m_sock->Connect(*addr, false);
}

void RadarFrame::OnSocketEvent(wxSocketEvent& event)
{
    switch ( event.GetSocketEvent() )
    {
        case wxSOCKET_INPUT:
            wxLogMessage("Input available on the socket");
            break;

        case wxSOCKET_LOST:
            wxLogMessage("Socket connection was unexpectedly lost.");
            UpdateStatusBar();
            break;

        case wxSOCKET_CONNECTION:
            wxLogMessage("... socket is now connected.");
            UpdateStatusBar();
            break;

        default:
            wxLogMessage("Unknown socket event!!!");
            break;
    }
}

void RadarFrame::OnTest(){
    // Disable socket menu entries (exception: Close Session)
    m_busy = true;
    UpdateStatusBar();

    // Tell the server which test we are running
    unsigned char c = 0xCE;
    m_sock->Write(&c, 1);

    // Here we use ReadMsg and WriteMsg to send messages with
    // a header with size information. Also, the reception is
    // event triggered, so we test input events as well.
    //
    // We need to set no flags here (ReadMsg and WriteMsg are
    // not affected by flags)

    m_sock->SetFlags(wxSOCKET_WAITALL);

    wxString s = "data";
    {
        // TODO:本部分发送数据内容（数据备选模式：wxString,json,xml）
        // 如果没有接入AIS或者其他设备，发送某种状态码
    }

    const wxScopedCharBuffer msg1(s.utf8_str());
    size_t len  = wxStrlen(msg1) + 1;
    wxCharBuffer msg2(wxStrlen(msg1));
    wxCharBuffer msgState[2] = {"1","2"};
    m_sock->WriteMsg(msg1, len);

    // Wait until data available (will also return if the connection is lost)
    m_sock->WaitForRead(2);

    if (m_sock->IsData())
    {
        m_sock->ReadMsg(msg2.data(), len);

        if (strcmp(msgState[0],msg2)){
            // 默认无返回数据，或者返回数据为空
        }
        else{
            // 存在返回船舶控制指令，采用espeak(TTS)文本语音指令播放指令语音内容
            do_play(msg2);
        }
    }
    else
        SetStatusText("Timeout ! Test 2 failed.", 1);

    m_busy = false;
    // UpdateStatusBar();
}

void RadarFrame::TTSPlaySound( wxCommandEvent &event) {
    if(m_soundButton->GetLabel() == "Open"){
        m_soundButton->SetLabel("Close");
    }
    else if (m_soundButton->GetLabel() == "Close"){
        m_soundButton->SetLabel("Open");
    }
}

void RadarFrame::Connect(){
    // m_Timer_TTS->Stop();   // timer can only be started from the main thread
    OnTest();
    // m_Timer_TTS->Start(RESTART);
}

void RadarFrame::TTSPlaySoundTimer( wxTimerEvent& event ) {
    //std::cout << "try send xml data" << std::endl;
    
    if(m_soundButton->GetLabel() == "Close"){
        // std::cout << "play sound function is open" << std::endl;
        // function 
        // 如果socket连接已经创建，发送融合后船舶位置数据，并且接收避碰算法处理结果，包含状态码和内容
        if (m_sock->IsConnected() ) {
           thread connectThread(&RadarFrame::Connect, this); // 线程实现
           connectThread.detach();
        }
        
        // TTS(Text to Speech)是通过命令行线程实现，需要提前配置espeak-ng以及其中文发音库.
        // const char *input = "espeak-ng  'TTS(Text to Speech)是通过命令行线程实现，需要提前配置espeak-ng以及其中文发音库.' -s 10 -p 15 -v zh";
        // char *result;
        // std::thread t(executeCMD, input, result);
        // t.detach();
    }
    else{
        // std::cout << "play sound function is close" << std::endl;
        // not connect with the alg model
        
    }
}

void RadarFrame::UpdateStatusBar()
{
#if wxUSE_STATUSBAR
    wxString s;

    if (!m_sock->IsConnected())
    {
        s = "Not connected";
        m_ConnectOptionButton->SetLabel("Open session");
        m_soundButton->SetLabel("Open");
        m_soundButton->Disable();
    }
    else
    {
#if wxUSE_IPV6
        wxIPV6address addr;
#else
        wxIPV4address addr;
#endif

        m_sock->GetPeer(addr);
        s.Printf("%s : %d", addr.Hostname(), addr.Service());
    }

    SetStatusText(s, 1);
#endif // wxUSE_STATUSBAR
}
#else
void RadarFrame::OnServerEvent(wxSocketEvent& event)
{
    wxString s = _("OnServerEvent: ");
    wxSocketBase *sock;

    switch(event.GetSocketEvent())
    {
        case wxSOCKET_CONNECTION : s.Append(_("wxSOCKET_CONNECTION\n")); break;
        default                  : s.Append(_("Unexpected event !\n")); break;
    }

    m_textCtrl1->AppendText(s);

    // Accept new connection if there is one in the pending
    // connections queue, else exit. We use Accept(false) for
    // non-blocking accept (although if we got here, there
    // should ALWAYS be a pending connection).

    sock = m_server->Accept(false);

    if (sock)
    {
        IPaddress addr;
        if ( !sock->GetPeer(addr) )
        {
        wxLogMessage("New connection from unknown client accepted.");
        }
        else
        {
        wxLogMessage("New client connection from %s:%u accepted",
                    addr.IPAddress(), addr.Service());
        }
    }
    else
    {
        wxLogMessage("Error: couldn't accept a new connection");
        return;
    }

    sock->SetEventHandler(*this, SOCKET_ID);
    sock->SetNotify(wxSOCKET_INPUT_FLAG | wxSOCKET_LOST_FLAG);
    sock->Notify(true);

    m_numClients++;
    UpdateStatusBar();
}

void RadarFrame::OnSocketEvent(wxSocketEvent& event)
{
    wxString s = _("OnSocketEvent: ");
    wxSocketBase *sock = event.GetSocket();

    // First, print a message
    switch(event.GetSocketEvent())
    {
        case wxSOCKET_INPUT : s.Append(_("wxSOCKET_INPUT\n")); break;
        case wxSOCKET_LOST  : s.Append(_("wxSOCKET_LOST\n")); break;
        default             : s.Append(_("Unexpected event !\n")); break;
    }

    m_textCtrl1->AppendText(s);

    // Now we process the event
    switch(event.GetSocketEvent())
    {
        case wxSOCKET_INPUT:
        {
            // We disable input events, so that the test doesn't trigger
            // wxSocketEvent again.
            sock->SetNotify(wxSOCKET_LOST_FLAG);

            // Which test are we going to run?
            unsigned char c;
            sock->Read(&c, 1);

            switch (c)
            {
                case 0xBE: SendData2Client(sock); break;
                case 0xCE: GetClientResult(sock); break;
                //case 0xDE: Test3(sock); break;
                default:
                wxLogMessage("Unknown test id received from client");
            }

            // Enable input events again.
            sock->SetNotify(wxSOCKET_LOST_FLAG | wxSOCKET_INPUT_FLAG);
            break;
        }
        case wxSOCKET_LOST:
        {
            m_numClients--;

            // Destroy() should be used instead of delete wherever possible,
            // due to the fact that wxSocket uses 'delayed events' (see the
            // documentation for wxPostEvent) and we don't want an event to
            // arrive to the event handler (the frame, here) after the socket
            // has been deleted. Also, we might be doing some other thing with
            // the socket at the same time; for example, we might be in the
            // middle of a test or something. Destroy() takes care of all
            // this for us.

            wxLogMessage("Deleting socket.");
            sock->Destroy();
            break;
        }
        default: ;
    }

    UpdateStatusBar();
}

// convenience functions

void RadarFrame::UpdateStatusBar()
{
#if wxUSE_STATUSBAR
    wxString s;
    s.Printf(_("%d clients connected"), m_numClients);
    SetStatusText(s, 1);
#endif // wxUSE_STATUSBAR
}

void RadarFrame::SendData2Client(wxSocketBase *sock)
{
    TestLogger logtest("SendData2Client");

    // Receive data from socket and send it back. We will first
    // get a byte with the buffer size, so we can specify the
    // exact size and use the wxSOCKET_WAITALL flag. Also, we
    // disabled input events so we won't have unwanted reentrance.
    // This way we can avoid the infamous wxSOCKET_BLOCK flag.

    sock->SetFlags(wxSOCKET_WAITALL);

    // Read the size
    unsigned char len;
    sock->Read(&len, 1);
    wxCharBuffer buf(len);

    // Read the data
    sock->Read(buf.data(), len);
    wxLogMessage("Got the data, sending it back");

    wxString data;
    data += wxString::Format(wxT("$!NDOS:OwnShip-%f-"), gLon)
            +wxString::Format(wxT("%f-"),gLat)
            +wxString::Format(wxT("%f-"),((gSog)*1852/3600))
            +wxString::Format(wxT("%f-"), gCog)
            +wxString::Format(wxT("%f-"), g_n_ownship_length_meters)
            +wxString::Format(wxT("%f\r\n"), g_n_ownship_beam_meters);
    
    ArrayOfPlugIn_AIS_Targets *current_targets = pPlugIn->GetAisTargets();
    
    for (auto it = current_targets->begin(); it != current_targets->end(); ++it )
    {
        
        
        //m_textCtrl1->AppendText(wxString::Format(wxT("---MMSI:%i"),td->MMSI)+wxString::Format(wxT("\tLon:%f"),td->Lon)+wxString::Format(wxT("\tLat:%f"),td->Lat));
        //m_textCtrl1->AppendText(_("\n"));

        // data += wxString::Format(wxT("$!NDAR:%i-"),(*it)->MMSI)
        //         +wxString::Format(wxT("%f-"),(*it)->Lon)
        //         +wxString::Format(wxT("%f-"),(*it)->Lat)
        //         +wxString::Format(wxT("%f-"),(((*it)->SOG)*1852/3600))
        //         +wxString::Format(wxT("%f\r\n"),(*it)->COG);
        data = "$!NDAR:12,12,12,12,12\r\n";
        
    }
    buf = data.ToUTF8();
    len = strlen(buf);
    sock->Write(&len, 1);
    sock->Write(buf, strlen(buf));
}

void RadarFrame::GetClientResult(wxSocketBase *sock)
{
    char buf[4096];

    TestLogger logtest("GetClientResult");

    // We don't need to set flags because ReadMsg and WriteMsg
    // are not affected by them anyway.

    // Read the message
    wxUint32 len = sock->ReadMsg(buf, sizeof(buf)).LastCount();
    if ( !len )
    {
        wxLogError("Failed to read message.");
        return;
    }

    wxLogMessage("Got \"%s\" from client.", wxString::FromUTF8(buf, len));
    wxLogMessage("Sending the data back");
    
    wxString s = "Get Message and Prase Right";
    unsigned int bufflen = s.size()+1;
    wxCharBuffer buff(bufflen);
    buff = s.ToUTF8();
    // Write it back
    sock->Write(&bufflen, 1);
    sock->Write(buff, bufflen);
}

void RadarFrame::Test3(wxSocketBase *sock)
{
    TestLogger logtest("Test 3");

    // This test is similar to the first one, but the len is
    // expressed in kbytes - this tests large data transfers.

    sock->SetFlags(wxSOCKET_WAITALL);

    // Read the size
    unsigned char len;
    sock->Read(&len, 1);
    wxCharBuffer buf(len*1024);

    // Read the data
    sock->Read(buf.data(), len * 1024);
    wxLogMessage("Got the data, sending it back");
    // buf = "警告";
    // Write it back
    buf = "前方船舶（南京号）距离本船500米，有碰撞危险，向右转向10度，减速";
    sock->Write(buf, len * 1024);
}
#endif

void RadarFrame::render(wxDC& dc)     {
    m_Timer->Start(RESTART);
    
    // Calculate the size based on paint area size, if smaller then the minimum
    // then the default minimum size applies
    int width  = max(m_pCanvas->GetSize().GetWidth(), (MIN_RADIUS)*2 );
    int height = max(m_pCanvas->GetSize().GetHeight(),(MIN_RADIUS)*2 );
    wxSize       size(width, height);
    wxPoint      center(width/2, height/2);
    int radius = max((min(width,height)/2),MIN_RADIUS);
    
    //    m_pCanvas->SetBackgroundColour (m_BgColour);
    renderRange(dc, center, size, radius);
    ArrayOfPlugIn_AIS_Targets *AisTargets = pPlugIn->GetAisTargets();
	if ( AisTargets ) {
        renderBoats(dc, center, size, radius, AisTargets);
    }
}


void RadarFrame::TrimAisField(wxString *fld) {
    assert(fld);
    while (fld->Right(1)=='@' || fld->Right(1)==' ') {
        fld->RemoveLast();
    }
}


void RadarFrame::renderBoats(wxDC& dc, wxPoint &center, wxSize &size, int radius, ArrayOfPlugIn_AIS_Targets *AisTargets ) {
    // Determine orientation
    double offset=pPlugIn->GetCog();
    if (m_pNorthUp->GetValue()) {
        offset=0;
    }

    // Get display settings
    bool   m_ShowMoored=pPlugIn->ShowMoored();
    double m_MooredSpeed=pPlugIn->GetMooredSpeed();
    bool   m_ShowCogArrows=pPlugIn->ShowCogArrows();
    int    m_CogArrowMinutes=pPlugIn->GetCogArrowMinutes();

    // Show other boats and base stations
    Target    dt;
//    ArrayOfPlugIn_AIS_Targets *AisTargets = pPlugIn->GetAisTargets();
    PlugIn_AIS_Target *t;
    ArrayOfPlugIn_AIS_Targets::iterator it;
    wxString  Name;

    // Set generic details for all targets
    dt.SetCanvas(center,radius, m_BgColour);
    dt.SetNavDetails(RangeData[m_pRange->GetSelection()], offset, m_ShowCogArrows, m_CogArrowMinutes);

    //TODO: 融合模块_1(统计加权融合模型) 数据map队列输入 map<<mmsi, class>, list<PlugIn_AIS_Target>>
    static std::map<std::pair<int, int>, std::list<PlugIn_AIS_Target> > Ais_Target;
    std::map<std::pair<int, int>, PlugIn_AIS_Target > Now_Ais_Target;
    
    for( it = (*AisTargets).begin(); it != (*AisTargets).end(); ++it ) {
        t = *it;
        // Only display well defined targets
        if (t->Range_NM>0.0 && t->Brg>0.0) {
            //t->MMSI, Name, t->Range_NM, t->Brg, t->COG, t->SOG, t->Class, t->alarm_state, t->ROTAIS
            std::pair<int, int> the_key(t->MMSI, t->Class);
            if (t->MID!=5 && t->MMSI){
                // 过滤ais静态数据
                Now_Ais_Target[the_key] = *t;
                if (Ais_Target.find(the_key)!=Ais_Target.end()){
                    // 去除utc时间相同的目标
                    if( t->Utc_hour!=Ais_Target[the_key].back().Utc_hour
                    ||t->Utc_min!=Ais_Target[the_key].back().Utc_min
                    ||t->Utc_sec!=Ais_Target[the_key].back().Utc_sec){
                        Ais_Target[the_key].push_back(*t);
                    }
                    if (Ais_Target[the_key].size()>10){
                        Ais_Target[the_key].pop_front();
                    }
                }
                else{
                    std::list<PlugIn_AIS_Target> newlist;
                    newlist.push_back(*t);
                    Ais_Target[the_key] = newlist;
                }
            }
        }
    }
    
    unsigned N = Ais_Target.size();
    int size_target[N];
    for (decltype(N) i = 0; i<N; ++i){
        size_target[i] = 0;
    }
    // if () //队列长度判断 距离本船距离 大于这一距离的船舶不需要考虑
    for (auto ais_it = Ais_Target.begin(); ais_it!=Ais_Target.end(); ++ais_it){
        
        if (size_target[distance(Ais_Target.begin(), ais_it)] == 1){
            std::cout << distance(Ais_Target.begin(), ais_it) << "当前目标已经融合\t";
            continue; //当前目标已经融合
        }
        
        auto ais_it_c = ais_it;
        while(++ais_it_c!=Ais_Target.end()){
            if (ais_it->first.second == ais_it_c->first.second || size_target[distance(Ais_Target.begin(), ais_it_c)]==1){
                continue; // 同类目标 或者 已经融合目标
            }
            else{
                //计算Radar_a和AIS_b的关联度
                //TODO:重复检测，减枝
                int m = 0;  //关联点数
                auto i = ais_it->second.begin();
                auto j = ais_it_c->second.begin();
                for(; i !=ais_it->second.end() && j != ais_it_c->second.end(); ++i, ++j)
                {
                    //距离的关联度函数
                    volatile double dt = fabs(i->Range_NM - j->Range_NM);
                    volatile double ad = exp(-0.1*(dt*dt)/(r1*r1));
                    //方位的关联度函数
                    volatile double ht = fabs(i->Brg - j->Brg);
                    volatile double ah = exp(-0.1*(ht*ht)/(r2*r2));
                    //速度的关联度函数
                    volatile double st = fabs(i->SOG - j->SOG);
                    volatile double as = exp(-0.5*(st*st)/(r3*r3));
                    //航向的关联度函数
                    volatile double ct = fabs(i->COG - j->COG);
                    volatile double ac = exp(-0.5*(ct*ct)/(r4*r4));

                    //该点的综合关联度,加权求和的方法
                    volatile double a = 0.35*ad + 0.35*ah + 0.15*as + 0.15*ac;
                    if(a > 0.6)
                    {
                        m++;
                    }
                }
                if( m > 8 ){

                    #if 1  // kalman
                    
                    #else  // 加权融合
                    // 航迹最后一个点 融合
                    std::cout << "Erase fusion point:" << std::endl;
                    size_target[std::distance(Ais_Target.begin(), ais_it)] = 1;
                    size_target[std::distance(Ais_Target.begin(), ais_it_c)] = 1;
                    std::cout << "Type:" << Now_Ais_Target[ais_it->first].Class << "\tName:" << Now_Ais_Target[ais_it->first].ShipName << Now_Ais_Target[ais_it->first].MMSI << std::endl;
                    std::cout << "Type:" << Now_Ais_Target[ais_it_c->first].Class << "\tName:" << Now_Ais_Target[ais_it_c->first].ShipName << Now_Ais_Target[ais_it_c->first].MMSI << std::endl;
                    // strcat(Now_Ais_Target[ais_it_c->first].ShipName, Now_Ais_Target[ais_it->first].ShipName);
                    
                    char buf[43];
                    strcpy(buf, Now_Ais_Target[ais_it_c->first].ShipName);
                    strcat(buf, Now_Ais_Target[ais_it->first].ShipName);
                    
                    
                    Now_Ais_Target.erase(ais_it->first);
                    //Now_Ais_Target.erase(ais_it_c->first);
                    
                    Now_Ais_Target[ais_it_c->first].Range_NM = a11*ais_it->second.back().Range_NM + a12*ais_it_c->second.back().Range_NM;
                    Now_Ais_Target[ais_it_c->first].Brg = a21*ais_it->second.back().Brg + a22*ais_it_c->second.back().Brg;
                    Now_Ais_Target[ais_it_c->first].SOG = a31*ais_it->second.back().SOG + a32*ais_it_c->second.back().SOG;
                    Now_Ais_Target[ais_it_c->first].COG = a41*ais_it->second.back().COG + a42*ais_it_c->second.back().COG;

                    #endif
                                       
                }
                
            }
        }
    }

    //TODO: 融合模块_2(分布式卡尔曼融合模型) by zhh
    {
        
    }
    
    for(auto it_ = (Now_Ais_Target).begin(); it_ != (Now_Ais_Target).end(); ++it_ ) {
        t        = &(it_->second);
        // Only display well defined targets
        if (t->Range_NM>0.0 && t->Brg>0.0) {
            if (m_ShowMoored 
                || t->Class == BASE_STATION
                ||(!m_ShowMoored && t->SOG > m_MooredSpeed)
            ) {
                Name     = wxString::From8BitData(t->ShipName);
                TrimAisField(&Name);
                dt.SetState(t->MMSI, Name, t->Range_NM, t->Brg, t->COG, t->SOG, 
                    t->Class, t->alarm_state, t->ROTAIS
                );
                dt.Render(dc);
            }
        }
    }

    // for( it = (*AisTargets).begin(); it != (*AisTargets).end(); ++it ) {
    //     t        = *it;
    //     // Only display well defined targets
    //     if (t->Range_NM>0.0 && t->Brg>0.0) {
    //         if (m_ShowMoored 
    //             || t->Class == BASE_STATION
    //             ||(!m_ShowMoored && t->SOG > m_MooredSpeed)
    //         ) {
    //             Name     = wxString::From8BitData(t->ShipName);
    //             TrimAisField(&Name);
    //             dt.SetState(t->MMSI, Name, t->Range_NM, t->Brg, t->COG, t->SOG, 
    //                 t->Class, t->alarm_state, t->ROTAIS
    //             );
    //             dt.Render(dc);
    //         }
    //     }
    // }
}


void RadarFrame::renderRange(wxDC& dc, wxPoint &center, wxSize &size, int radius) {
    // Draw the circles
    dc.SetBackground(wxBrush(m_BgColour));
    dc.Clear();
    dc.SetBrush(wxBrush(wxColour(0,0,0),wxTRANSPARENT));
    dc.SetPen( wxPen( wxColor(128,128,128), 1, wxSOLID ) );
	dc.DrawCircle( center, radius);
    dc.SetPen( wxPen( wxColor(128,128,128), 1, wxDOT ) );
    dc.DrawCircle( center, radius*0.75 );
    dc.DrawCircle( center, radius*0.50 );
    dc.DrawCircle( center, radius*0.25 );
    dc.SetPen( wxPen( wxColor(128,128,128), 2, wxSOLID ) );
    dc.DrawCircle( center, 10 );

    // Draw the crosshairs
    dc.SetPen( wxPen( wxColor(128,128,128), 1, wxDOT ) );
    dc.DrawLine( size.GetWidth()/2,0, size.GetWidth()/2, size.GetHeight());
    dc.DrawLine( 0,size.GetHeight()/2, size.GetWidth(), size.GetHeight()/2);

    // Draw the range description
    wxFont fnt = dc.GetFont();
    fnt.SetPointSize(14);
    int fh=fnt.GetPointSize();
    dc.SetFont(fnt);
    float Range=RangeData[m_pRange->GetSelection()];
    dc.DrawText(wxString::Format(wxT("%s %2.2f"), _("Range"),Range  ), 0, 0); 
 //   dc.DrawText(wxString::Format(wxT("%s %2.2f"), _("Ring "), Range/4), 0, fh+TEXT_MARGIN); 

    // Draw the orientation info
	wxString dir;
	if (m_pNorthUp->GetValue()) {
		dir=_("North Up");
        // Draw north, east, south and west indicators
        dc.SetTextForeground(wxColour(128,128,128));
        dc.DrawText(_("N"), size.GetWidth()/2 + 5, 0);
        dc.DrawText(_("S"), size.GetWidth()/2 + 5, size.GetHeight()-dc.GetCharHeight());
        dc.DrawText(_("W"), 5, size.GetHeight()/2 - dc.GetCharHeight());
        dc.DrawText(_("E"), size.GetWidth() - 7 - dc.GetCharWidth(), size.GetHeight()/2 - dc.GetCharHeight());
    } else {
        dir=_("Course Up"); 
        // Display our own course at to top
        double offset=pPlugIn->GetCog();
        dc.SetTextForeground(wxColour(128,128,128));
        int cpos=0;
        dc.DrawText(wxString::Format(_T("%3.0f\u00B0"),offset), size.GetWidth()/2 - dc.GetCharWidth()*2, cpos);
    }
    dc.SetTextForeground(wxColour(0,0,0));
    dc.DrawText(dir,  size.GetWidth()-dc.GetCharWidth()*dir.Len()-fh-TEXT_MARGIN, 0); 
    if (m_pBearingLine->GetValue()) {
        // Display and estimated bearing line
        int x = center.x;
        int y = center.y;
        double angle = m_Ebl *(double)((double)3.141592653589/(double)180.);
        x += sin(angle) * (radius + 20);
        y -= cos(angle) * (radius + 20);
        dc.DrawLine(center.x, center.y, x, y);
        int tx = center.x + sin(angle) * (radius - 20) - dc.GetCharWidth() * 1.5;
        int ty = center.y - cos(angle) * (radius - 20);
        double offset=0.;
        if ( !m_pNorthUp->GetValue() ) {
            offset = pPlugIn->GetCog();
        }
        dc.SetTextForeground(wxColour(128,128,128));
        dc.DrawText(wxString::Format(_T("%3.1d\u00B0"),(int)(m_Ebl+offset)%360),tx,ty);
    }
}