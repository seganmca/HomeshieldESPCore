#include "ProvisioningService.h"
#include "Configuration.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>

#include "Debug.h"

namespace
{
    const IPAddress PortalIp(
        192, 168, 4, 1);

    const IPAddress PortalGateway(
        192, 168, 4, 1);

    const IPAddress PortalSubnet(
        255, 255, 255, 0);


    void EnableApDnsOffer(
        const IPAddress& apIp)
    {
        auto netif =
            esp_netif_get_handle_from_ifkey(
                "WIFI_AP_DEF");

        if (netif == nullptr)
        {
            DEBUG_LOG(
                "[PORTAL] AP netif NOT FOUND - DNS offer not configured.");

            return;
        }

        esp_netif_dns_info_t dns = {};

        dns.ip.type =
            ESP_IPADDR_TYPE_V4;

        dns.ip.u_addr.ip4.addr =
            static_cast<uint32_t>(apIp);

        auto stopResult =
            esp_netif_dhcps_stop(netif);

        DEBUG_VALUE(
            "[PORTAL] dhcps_stop",
            esp_err_to_name(stopResult));

        uint8_t offerDns = 0x02;

        auto optionResult =
            esp_netif_dhcps_option(
                netif,
                ESP_NETIF_OP_SET,
                ESP_NETIF_DOMAIN_NAME_SERVER,
                &offerDns,
                sizeof(offerDns));

        DEBUG_VALUE(
            "[PORTAL] dhcps_option(DNS)",
            esp_err_to_name(optionResult));

        auto dnsResult =
            esp_netif_set_dns_info(
                netif,
                ESP_NETIF_DNS_MAIN,
                &dns);

        DEBUG_VALUE(
            "[PORTAL] set_dns_info",
            esp_err_to_name(dnsResult));

        auto startResult =
            esp_netif_dhcps_start(netif);

        DEBUG_VALUE(
            "[PORTAL] dhcps_start",
            esp_err_to_name(startResult));

        esp_netif_dns_info_t readBack = {};

        if (esp_netif_get_dns_info(
                netif,
                ESP_NETIF_DNS_MAIN,
                &readBack) == ESP_OK)
        {
            IPAddress offered(
                readBack.ip.u_addr.ip4.addr);

            DEBUG_VALUE(
                "[PORTAL] DHCP will offer DNS",
                offered);
        }
        else
        {
            DEBUG_LOG(
                "[PORTAL] Could not read back AP DNS info.");
        }
    }


    const char PortalPage[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>HomeShield Setup</title>
<style>
:root{--bg:#0f172a;--card:#ffffff;--ink:#0f172a;--muted:#64748b;--line:#e2e8f0;--brand:#2563eb;--ok:#16a34a;--err:#dc2626}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif}
.wrap{max-width:460px;margin:0 auto;padding:24px 16px 40px}
.brand{display:flex;align-items:center;gap:10px;color:#fff;margin:8px 0 18px}
.brand svg{width:30px;height:30px;flex:none}
.brand h1{font-size:19px;margin:0;font-weight:600;letter-spacing:.2px}
.card{background:var(--card);border-radius:16px;padding:20px;box-shadow:0 12px 30px rgba(2,6,23,.35)}
.card h2{margin:0 0 6px;font-size:18px}
.lead{margin:0 0 18px;color:var(--muted);font-size:14px}
label{display:block;font-size:13px;font-weight:600;margin:0 0 6px;color:#334155}
.row{margin-bottom:16px}
input[type=text],input[type=password]{width:100%;padding:12px 14px;border:1px solid var(--line);border-radius:10px;font-size:16px;background:#f8fafc}
input:focus{outline:2px solid var(--brand);outline-offset:1px;background:#fff}
.pw{position:relative}
.pw input{padding-right:64px}
.pw button{position:absolute;right:6px;top:6px;bottom:6px;border:0;background:transparent;color:var(--brand);font-size:13px;font-weight:600;padding:0 10px;cursor:pointer}
.list{border:1px solid var(--line);border-radius:10px;overflow:hidden;max-height:230px;overflow-y:auto}
.net{display:flex;align-items:center;gap:10px;width:100%;padding:12px 14px;border:0;border-bottom:1px solid var(--line);background:#fff;font-size:15px;text-align:left;cursor:pointer;color:var(--ink)}
.net:last-child{border-bottom:0}
.net.sel{background:#eff6ff}
.net .nm{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.net .mt{color:var(--muted);font-size:12px;display:flex;align-items:center;gap:5px}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:3px;background:var(--line);border-radius:1px}
.bars i:nth-child(1){height:4px}.bars i:nth-child(2){height:7px}.bars i:nth-child(3){height:10px}.bars i:nth-child(4){height:14px}
.bars i.on{background:var(--brand)}
.hint{color:var(--muted);font-size:12px;margin:8px 2px 0}
.linkbtn{background:none;border:0;color:var(--brand);font-size:13px;font-weight:600;padding:0;cursor:pointer}
.bar{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
button.go{width:100%;padding:14px;border:0;border-radius:10px;background:var(--brand);color:#fff;font-size:16px;font-weight:600;cursor:pointer}
button.go:disabled{opacity:.55}
.state{text-align:center;padding:8px 0 4px}
.spin{width:34px;height:34px;margin:6px auto 14px;border:3px solid var(--line);border-top-color:var(--brand);border-radius:50%;animation:sp 1s linear infinite}
@keyframes sp{to{transform:rotate(360deg)}}
.dot{width:44px;height:44px;border-radius:50%;margin:4px auto 14px;display:flex;align-items:center;justify-content:center;color:#fff;font-size:24px}
.dot.ok{background:var(--ok)}.dot.err{background:var(--err)}
.msg{color:var(--muted);font-size:14px;margin:0 0 16px}
.foot{color:#94a3b8;font-size:12px;text-align:center;margin-top:18px}
.hide{display:none}
</style>
</head>
<body>
<div class="wrap">
  <div class="brand">
    <svg viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2l8 3.5v6c0 5-3.4 9.3-8 10.5-4.6-1.2-8-5.5-8-10.5v-6z"/><path d="M9 12l2 2 4-4"/></svg>
    <h1>HomeShield</h1>
  </div>

  <div class="card" id="form">
    <h2>Connect to your Wi-Fi</h2>
    <p class="lead">Choose your home Wi-Fi network and enter its password. Your HomeShield device will use it to stay online.</p>

    <div class="row">
      <div class="bar"><label>Available networks</label><button class="linkbtn" id="rescan" type="button">Rescan</button></div>
      <div class="list" id="list"><div style="padding:14px;color:#64748b;font-size:14px">Scanning&hellip;</div></div>
      <p class="hint" id="manualHint"><button class="linkbtn" id="manual" type="button">Enter network name manually</button></p>
    </div>

    <div class="row hide" id="ssidRow">
      <label for="ssid">Network name (SSID)</label>
      <input type="text" id="ssid" autocapitalize="none" autocorrect="off" spellcheck="false" placeholder="MyHomeWiFi">
    </div>

    <div class="row">
      <label for="pwd">Wi-Fi password</label>
      <div class="pw">
        <input type="password" id="pwd" autocapitalize="none" autocorrect="off" spellcheck="false" placeholder="Password">
        <button type="button" id="toggle">Show</button>
      </div>
    </div>

    <button class="go" id="go">Connect</button>
    <p class="hint" id="err" style="color:#dc2626"></p>
  </div>

  <div class="card hide" id="busy">
    <div class="state">
      <div class="spin"></div>
      <h2 id="bt">Connecting&hellip;</h2>
      <p class="msg" id="bm">Joining your Wi-Fi network. This can take up to half a minute.</p>
    </div>
  </div>

  <div class="card hide" id="done">
    <div class="state">
      <div class="dot ok">&#10003;</div>
      <h2>Connected</h2>
      <p class="msg" id="dm">Your device is now on your home Wi-Fi and is restarting. You can reconnect your phone to your normal network.</p>
    </div>
  </div>

  <div class="card hide" id="fail">
    <div class="state">
      <div class="dot err">!</div>
      <h2>Couldn't connect</h2>
      <p class="msg" id="fm">Please check the password and try again.</p>
      <button class="go" id="retry">Try again</button>
    </div>
  </div>

  <p class="foot">HomeShield device setup &middot; 192.168.4.1</p>
</div>

<script>
var sel="", open=false, poll=null;

function $(i){
  return document.getElementById(i)
}

function show(id){
  ["form","busy","done","fail"].forEach(function(x){
    $(x).classList.toggle("hide",x!==id)
  })
}

function bars(r){
  var n=r>=-55?4:r>=-65?3:r>=-75?2:1,h="";
  for(var i=1;i<=4;i++)
    h+='<i class="'+(i<=n?"on":"")+'"></i>';
  return '<span class="bars">'+h+'</span>'
}

function esc(s){
  return s.replace(/[&<>"]/g,function(c){
    return {"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]
  })
}

function scan(){
  $("list").innerHTML='<div style="padding:14px;color:#64748b;font-size:14px">Scanning&hellip;</div>';

  fetch("/scan"+(arguments[0]?"?refresh=1":""))
    .then(function(r){return r.json()})
    .then(function(d){
      if(d.status==="scanning"){
        setTimeout(scan,1200);
        return
      }

      var n=d.networks||[];

      if(!n.length){
        $("list").innerHTML='<div style="padding:14px;color:#64748b;font-size:14px">No networks found. Tap Rescan or enter the name manually.</div>';
        return
      }

      $("list").innerHTML=n.map(function(x){
        return '<button type="button" class="net'+
          (x.ssid===sel?" sel":"")+
          '" data-s="'+esc(x.ssid)+'"><span class="nm">'+
          esc(x.ssid)+
          '</span><span class="mt">'+
          (x.secure?"&#128274;":"")+
          bars(x.rssi)+
          '</span></button>'
      }).join("");

      Array.prototype.forEach.call(
        $("list").querySelectorAll(".net"),
        function(b){
          b.onclick=function(){
            sel=b.getAttribute("data-s");
            $("ssid").value=sel;
            $("err").textContent="";

            Array.prototype.forEach.call(
              $("list").querySelectorAll(".net"),
              function(o){
                o.classList.remove("sel")
              });

            b.classList.add("sel");
            $("pwd").focus()
          }
        });
    })
    .catch(function(){
      setTimeout(scan,2000)
    })
}

$("rescan").onclick=scan;

$("manual").onclick=function(){
  $("ssidRow").classList.remove("hide");
  $("manualHint").classList.add("hide");
  $("ssid").focus()
};

$("toggle").onclick=function(){
  open=!open;
  $("pwd").type=open?"text":"password";
  $("toggle").textContent=open?"Hide":"Show"
};

$("retry").onclick=function(){
  show("form");
  scan()
};

$("go").onclick=function(){
  var s=$("ssid").value||sel;

  if(!s){
    $("err").textContent="Select a network or enter its name.";
    return
  }

  $("go").disabled=true;
  $("err").textContent="";

  var b=
    "ssid="+encodeURIComponent(s)+
    "&password="+encodeURIComponent($("pwd").value);

  fetch(
    "/save",
    {
      method:"POST",
      headers:{
        "Content-Type":
          "application/x-www-form-urlencoded"
      },
      body:b
    })
    .then(function(){
      show("busy");
      $("bm").textContent=
        'Joining "'+s+'". This can take up to half a minute.';
      track()
    })
    .catch(function(){
      $("go").disabled=false;
      $("err").textContent=
        "Could not reach the device. Please try again."
    });
};

function track(){
  if(poll)
    clearInterval(poll);

  var misses=0;

  poll=setInterval(function(){
    fetch(
      "/status",
      {
        cache:"no-store"
      })
      .then(function(r){
        return r.json()
      })
      .then(function(d){
        misses=0;

        if(d.state==="connected"){
          clearInterval(poll);

          $("dm").textContent=
            "Your device joined \""+
            d.ssid+
            "\" and is restarting. Reconnect your phone to your normal network.";

          show("done")
        }
        else if(d.state==="failed"){
          clearInterval(poll);

          $("fm").textContent=
            d.reason||
            "Please check the password and try again.";

          $("go").disabled=false;
          show("fail")
        }
      })
      .catch(function(){
        misses++;

        if(misses>12){
          clearInterval(poll);

          $("dm").textContent=
            "The device left setup mode, which usually means it joined your Wi-Fi. Reconnect your phone to your normal network.";

          show("done")
        }
      });
  },1500)
}

scan();
</script>
</body>
</html>)HTML";
}


ProvisioningService::ProvisioningService(
    StorageService& storageService)
    : _storageService(storageService)
{
}


void ProvisioningService::Begin()
{
    if (_storageService.HasWifiCredentials())
    {
        DEBUG_LOG("WiFi credentials found.");

        ConnectToWifi();

        return;
    }

    DEBUG_LOG("No WiFi credentials.");

    StartCaptivePortal();
}


void ProvisioningService::StartCaptivePortal()
{
    WiFi.persistent(false);

    RunInitialScan();

    WiFi.mode(WIFI_AP);

    WiFi.setTxPower(
        WIFI_POWER_8_5dBm);

    WiFi.softAPConfig(
        PortalIp,
        PortalGateway,
        PortalSubnet);

    if (WiFi.softAP(ApSsid))
    {
        DEBUG_LOG(
            "Access Point started.");
    }
    else
    {
        DEBUG_LOG(
            "Failed to start Access Point.");
    }

    delay(200);

    EnableApDnsOffer(
        WiFi.softAPIP());

    _dnsServer.setTTL(0);

    _dnsServer.setErrorReplyCode(
        DNSReplyCode::NoError);

    if (_dnsServer.start(
            DnsPort,
            "*",
            WiFi.softAPIP()))
    {
        DEBUG_LOG(
            "DNS server started.");
    }
    else
    {
        DEBUG_LOG(
            "Failed to start DNS server.");
    }

    ConfigureRoutes();

    static const char* headerKeys[] =
    {
        "User-Agent"
    };

    _server.collectHeaders(
        headerKeys,
        1);

    _server.begin();

    _portalActive = true;

    _state =
        ProvisioningState::Idle;

    DEBUG_LOG(
        "Web server started.");

    DEBUG_VALUE(
        "AP IP : ",
        WiFi.softAPIP());

    DEBUG_LOG(
        "[PORTAL] Waiting for a phone to join and probe...");
}


void ProvisioningService::Loop()
{
    if (!_portalActive)
        return;

    _dnsServer.processNextRequest();

    _server.handleClient();

#if DEBUG
    if (millis() - _lastPortalLog >= 5000)
    {
        _lastPortalLog = millis();

        DEBUG_VALUE(
            "[PORTAL] clients associated",
            WiFi.softAPgetStationNum());
    }
#endif

    UpdateConnectionAttempt();
}


void ProvisioningService::ConfigureRoutes()
{
    _server.on(
        "/",
        [this]()
        {
            HandleRoot();
        });


    _server.on(
        "/save",
        HTTP_POST,
        [this]()
        {
            HandleSave();
        });


    _server.on(
        "/scan",
        [this]()
        {
            HandleScan();
        });


    _server.on(
        "/status",
        [this]()
        {
            HandleStatus();
        });


    const char* detectionPaths[] =
    {
        "/generate_204",
        "/gen_204",
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/connecttest.txt",
        "/ncsi.txt",
        "/redirect",
        "/fwlink",
        "/canonical.html",
        "/success.txt"
    };


    for (auto path : detectionPaths)
    {
        _server.on(
            path,
            [this]()
            {
                HandleCaptiveRedirect();
            });
    }


    _server.onNotFound(
        [this]()
        {
            HandleCaptiveRedirect();
        });
}


void ProvisioningService::LogRequest(
    const char* tag)
{
#if DEBUG
    Serial.print("[PORTAL] ");
    Serial.print(tag);
    Serial.print(" method=");

    Serial.print(
        _server.method() == HTTP_GET
            ? "GET"
            : _server.method() == HTTP_POST
                ? "POST"
                : "OTHER");

    Serial.print(" host=");
    Serial.print(
        _server.hostHeader());

    Serial.print(" uri=");
    Serial.print(
        _server.uri());

    Serial.print(" ua=");
    Serial.println(
        _server.header(
            "User-Agent"));
#else
    (void)tag;
#endif
}


void ProvisioningService::HandleCaptiveRedirect()
{
    LogRequest("probe");

    HandleRoot();
}


void ProvisioningService::HandleRoot()
{
    LogRequest("root");

    _server.sendHeader(
        "Cache-Control",
        "no-cache, no-store, must-revalidate");

    _server.sendHeader(
        "Pragma",
        "no-cache");

    _server.send_P(
        200,
        "text/html",
        PortalPage);

    DEBUG_VALUE(
        "[PORTAL] served setup page, bytes",
        strlen_P(PortalPage));

    DEBUG_VALUE(
        "[PORTAL] client still connected",
        _server.client().connected());
}


String ProvisioningService::BuildScanJson(
    int count)
{
    String json =
        "{\"status\":\"done\",\"networks\":[";

    auto limit =
        count > 20
            ? 20
            : count;

    bool first = true;

    for (int i = 0; i < limit; i++)
    {
        auto ssid =
            WiFi.SSID(i);

        if (ssid.length() == 0)
            continue;

        if (!first)
            json += ",";

        first = false;

        json += "{\"ssid\":\"";
        json += Escape(ssid);
        json += "\",\"rssi\":";
        json += String(WiFi.RSSI(i));
        json += ",\"secure\":";
        json +=
            WiFi.encryptionType(i)
                == WIFI_AUTH_OPEN
                    ? "false"
                    : "true";
        json += "}";
    }

    json += "]}";

    WiFi.scanDelete();

    return json;
}


void ProvisioningService::RunInitialScan()
{
    WiFi.mode(
        WIFI_AP_STA);

    auto count =
        WiFi.scanNetworks(
            false,
            false);

    DEBUG_VALUE(
        "[PORTAL] initial scan networks",
        count);

    _scanJson =
        count > 0
            ? BuildScanJson(count)
            : String(
                "{\"status\":\"done\",\"networks\":[]}");

    WiFi.mode(WIFI_AP);
}


void ProvisioningService::HandleScan()
{
    LogRequest("scan");

    auto result =
        WiFi.scanComplete();

    if (result ==
        WIFI_SCAN_RUNNING)
    {
        _server.send(
            200,
            "application/json",
            "{\"status\":\"scanning\"}");

        return;
    }


    if (_scanPending &&
        result >= 0)
    {
        _scanJson =
            BuildScanJson(result);

        _scanPending = false;

        WiFi.mode(WIFI_AP);
    }


    if (_server.hasArg("refresh") &&
        !_scanPending)
    {
        if (WiFi.getMode() !=
            WIFI_AP_STA)
        {
            WiFi.mode(
                WIFI_AP_STA);
        }

        WiFi.scanNetworks(
            true,
            false);

        _scanPending = true;

        _server.send(
            200,
            "application/json",
            "{\"status\":\"scanning\"}");

        return;
    }


    if (_scanJson.length() == 0)
    {
        _scanJson =
            "{\"status\":\"done\",\"networks\":[]}";
    }

    _server.send(
        200,
        "application/json",
        _scanJson);
}


void ProvisioningService::HandleStatus()
{
    LogRequest("status");

    _server.sendHeader(
        "Cache-Control",
        "no-cache, no-store, must-revalidate");

    _server.send(
        200,
        "application/json",
        BuildStatusJson());
}


String ProvisioningService::BuildStatusJson()
{
    String state = "idle";

    switch (_state)
    {
        case ProvisioningState::Connecting:
            state = "connecting";
            break;

        case ProvisioningState::Connected:
            state = "connected";
            break;

        case ProvisioningState::Failed:
            state = "failed";
            break;

        default:
            break;
    }


    String json =
        "{\"state\":\"" +
        state +
        "\"";

    json +=
        ",\"ssid\":\"" +
        Escape(_pendingSsid) +
        "\"";


    if (_state ==
        ProvisioningState::Connected)
    {
        json +=
            ",\"ip\":\"" +
            WiFi.localIP().toString() +
            "\"";
    }


    if (_state ==
        ProvisioningState::Failed)
    {
        json +=
            ",\"reason\":\"" +
            Escape(_failureReason) +
            "\"";
    }


    json += "}";

    return json;
}


void ProvisioningService::HandleSave()
{
    LogRequest("save");

    auto ssid =
        _server.arg("ssid");

    auto password =
        _server.arg("password");

    DEBUG_VALUE(
        "SSID : ",
        ssid);

    DEBUG_VALUE(
        "Password : ",
        password);


    if (ssid.length() == 0)
    {
        _server.send(
            400,
            "application/json",
            "{\"state\":\"failed\",\"reason\":\"Network name is required.\"}");

        return;
    }


    _pendingSsid =
        ssid;

    _pendingPassword =
        password;

    BeginConnectionAttempt();


    _server.send(
        200,
        "application/json",
        BuildStatusJson());
}


void ProvisioningService::BeginConnectionAttempt()
{
    DEBUG_VALUE(
        "Connecting to ",
        _pendingSsid);

    _failureReason = "";

    _state =
        ProvisioningState::Connecting;

    _connectStartedAt =
        millis();


    if (WiFi.scanComplete() ==
        WIFI_SCAN_RUNNING)
    {
        WiFi.scanDelete();
    }


    WiFi.mode(
        WIFI_AP_STA);


    WiFi.disconnect(
        false,
        true);

    delay(50);


    WiFi.begin(
        _pendingSsid.c_str(),
        _pendingPassword.c_str());
}


void ProvisioningService::UpdateConnectionAttempt()
{
    if (_state ==
        ProvisioningState::Connected)
    {
        if (millis() -
            _connectedAt >=
            RestartDelay)
        {
            DEBUG_LOG(
                "Restarting after provisioning.");

            ESP.restart();
        }

        return;
    }


    if (_state !=
        ProvisioningState::Connecting)
    {
        return;
    }


    if (WiFi.status() ==
        WL_CONNECTED)
    {
        DEBUG_LOG(
            "WiFi Connected.");

        DEBUG_VALUE(
            "IP Address : ",
            WiFi.localIP());


        _storageService.SaveWifiCredentials(
            _pendingSsid,
            _pendingPassword);


        _state =
            ProvisioningState::Connected;

        _connectedAt =
            millis();

        return;
    }


    if (millis() -
        _connectStartedAt <
        ConnectTimeout)
    {
        return;
    }


    DEBUG_LOG(
        "Provisioning connection failed.");


    auto status =
        WiFi.status();


    _failureReason =
        status ==
            WL_NO_SSID_AVAIL
            ? "That network was not found. Move the device closer and try again."
            : "Could not join the network. Please check the password and try again.";


    WiFi.disconnect(
        false,
        true);


    _state =
        ProvisioningState::Failed;
}


String ProvisioningService::Escape(
    const String& value)
{
    String out;

    out.reserve(
        value.length() + 8);


    for (unsigned int i = 0;
         i < value.length();
         i++)
    {
        auto c =
            value.charAt(i);


        switch (c)
        {
            case '"':
                out += "\\\"";
                break;

            case '\\':
                out += "\\\\";
                break;

            case '\n':
                out += "\\n";
                break;

            case '\r':
                out += "\\r";
                break;

            case '\t':
                out += "\\t";
                break;

            default:

                if ((unsigned char)c < 0x20)
                {
                    char buf[7];

                    snprintf(
                        buf,
                        sizeof(buf),
                        "\\u%04x",
                        (unsigned)(
                            unsigned char)c);

                    out += buf;
                }
                else
                {
                    out += c;
                }

                break;
        }
    }

    return out;
}


void ProvisioningService::ConnectToWifi()
{
    auto ssid =
        _storageService.GetWifiSsid();

    auto password =
        _storageService.GetWifiPassword();


    DEBUG_VALUE(
        "Connecting to ",
        ssid);


    _pendingSsid =
        ssid;


    _pendingPassword =
        password;


    _failureReason = "";


    _state =
        ProvisioningState::Connecting;


    _connectStartedAt =
        millis();


    WiFi.mode(
        WIFI_STA);

    WiFi.setTxPower(
        WIFI_POWER_8_5dBm);


    WiFi.begin(
        ssid.c_str(),
        password.c_str());


    DEBUG_LOG(
        "WiFi connection attempt started.");
}