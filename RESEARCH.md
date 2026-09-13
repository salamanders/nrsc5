# **Engineering an Autonomous Offline Media Distribution Hotspot: Architecture, Captive Portal Traversal, and Mobile Client Interoperability**

Operating an air-gapped, offline digital distribution appliance on a single-board computer introduces networking complications across modern mobile operating systems. Smartphones and tablets are engineered under the assumption that wireless access points provide a path to the public internet1. When a client device associates with a network lacking an active upstream route, the operating system initiates automated captive portal discovery routines, evaluates multi-interface routing metrics, and often imposes severe restrictions on local browser execution1. Constructing an autonomous digital distribution server—such as an unmetered, high-capacity art server—requires an architecture that mitigates sandboxed webviews, bypasses cellular domain name system (DNS) fallback, and delivers uncorrupted binary assets directly to user-accessible local storage3.

## **Access Point Security and Layer-2 Association Paradigms**

The primary design choice at the physical and data-link layers involves selecting between an open Service Set Identifier (SSID) and a secured Wi-Fi Protected Access 2 (WPA2-PSK) broadcast6. While an open network initially appears to offer the lowest barrier to entry, it introduces substantial network instability in environments with ambient foot traffic1. Modern smartphones continuously poll for known and open broadcast profiles. In a populated area, dozens of passing mobile devices will opportunistically associate with an open network in the background1. Because the local access point provides no global internet uplink, these opportunistic associations disrupt background cellular connections on passing devices while concurrently flooding the Raspberry Pi with background transmission control protocol (TCP) traffic, captive network probes, and address resolution protocol (ARP) queries1. This ambient traffic quickly exhausts the Dynamic Host Configuration Protocol (DHCP) lease pool and degrades the limited radio frequency (RF) bandwidth of the onboard wireless chipset6.

Enforcing WPA2-Personal authentication resolves this challenge by requiring deliberate user interaction, ensuring that only users actively intending to interact with the media hub associate with the radio interface6. Although manual passphrase entry typically introduces friction, this obstacle is overcome by encoding the Wi-Fi association parameters into an ISO/IEC 18004:2015-compliant Quick Response (QR) code displayed physically at the venue. Scanning this standardized string using the native camera application on iOS or Android automatically extracts the SSID and pre-shared key, prompting the operating system to authenticate and associate with the access point without requiring navigation through system settings6.

## **Mobile Captive Network Probes and Download Sandbox Restrictions**

Immediately following wireless association and the acquisition of an IPv4 address, mobile operating systems dispatch unencrypted HTTP probe requests to vendor-controlled hostnames to verify global internet access2. If the operating system receives a response that does not exactly match its expected cryptographic or string baseline, it assumes that network access is constrained by a captive portal1.

&nbsp;

| Platform | Diagnostic Hostname and URI | Expected Network Response | Heuristic Trigger for Captive State |
| :---- | :---- | :---- | :---- |
| **Apple iOS / macOS** | captive.apple.com/hotspot-detect.html | HTTP 200 with \<TITLE\>Success\</TITLE\> | HTTP 302 redirect, non-matching HTML, or TCP timeout1 |
| **Google Android** | connectivitycheck.gstatic.com/generate\_204 | HTTP 204 No Content | HTTP 200 with payload, HTTP 302 redirect, or TCP timeout10 |
| **Microsoft Windows** | www.msftconnecttest.com/connecttest.txt | HTTP 200 with text Microsoft Connect Test | Divergent string payload, redirect, or TCP timeout11 |
| **Mozilla Firefox** | detectportal.firefox.com/success.txt | HTTP 200 with text success | Divergent string payload or HTTP 302 redirect1 |

When Apple devices detect a captive state, the core networking daemon invokes the Captive Network Assistant (CaptiveNetworkAssistant.app), which displays an isolated, modal WebSheet view4. This mini-browser operates under severe administrative constraints: it runs within a restricted sandbox, lacks access to Safari’s shared storage or authentication credentials, prevents multi-tab navigation, hides the address bar, and disables WebKit file-download delegates4.

If a web server inside this modal issues an HTTP response containing a Content-Disposition: attachment header or attempts to initiate an HTML5 programmatic download, the Captive Network Assistant ignores the directive or aborts the transaction entirely4. Moreover, if the user attempts to close this modal via the top-corner "Cancel" control, the iOS networking subsystem treats the action as an intentional network abandonment, severing the 802.11 association and returning the device to cellular data1.

On Android platforms, captive portal handling is managed by the system application CaptivePortalLogin16. While Android permits wider filesystem access than iOS and does not universally block file persistence inside its captive webview, modern implementations trigger persistent notifications warning that the network lacks internet connectivity10. Unless the user explicitly interacts with the system overflow menu to select "Use this network as is," background synchronization can terminate unexpectedly, or the device may divert traffic over its cellular radio interface16.

Emerging specifications such as RFC 8908 and RFC 8910 define an API-driven captive portal architecture via DHCP Option 1142. However, these standards require mutual Transport Layer Security (TLS) with certificates signed by a globally trusted Certificate Authority19. Because an air-gapped Raspberry Pi cannot establish public domain trust or resolve public Certificate Revocation Lists without an active WAN uplink, modern RFC-based captive frameworks are structurally unviable for standalone offline hardware19.

## **Architectural Traversal: Captive Suppression Versus Interactive Breakout**

To circumvent the download restrictions imposed by captive network sandboxes, the server must either programmatically navigate the user out of the captive state or suppress captive portal detection entirely12.

&nbsp;

| Architectural Dimension | Strategy A: Captive Suppression (Link-Local Gateway Omission) | Strategy B: Interactive CNA Breakout (Two-Stage State Machine) | Strategy C: Full Captive Interception (Wildcard DNS Spoofing) |
| :---- | :---- | :---- | :---- |
| **iOS User Experience** | Connects to WLAN; accesses server via Safari with full filesystem rights3 | Displays splash modal; user taps action, then manual "Done" dismisses sheet1 | Trapped inside CNA sandbox; binary file downloads are blocked4 |
| **Android Behavior** | Preserves cellular data; suppresses captive modal while allowing local access3 | Standard captive prompt; marks link authorized upon probe pass15 | Modal displayed; unstable download handling across device vendors16 |
| **Cellular Continuity** | Active; background push notifications and messaging remain functional3 | Temporarily interrupted until the breakout state machine finishes1 | Severed entirely; all outbound packets sink into non-routing AP24 |
| **DNS Resolution Path** | Direct numeric IPv4 addressing; bypasses DNS subsystem completely26 | Employs DNS spoofing transitioned to local records upon validation15 | Global DNS interception maps all hostnames to local gateway24 |
| **Long-Term Fragility** | Low; leverages foundational RFC 2132 host routing rules27 | High; vulnerable to heuristic shifts in vendor probe mechanics1 | Fails the primary functional objective on Apple devices4 |

### **Mechanism of Strategy A: Captive Suppression via Gateway Nullification**

Strategy A avoids triggering the captive portal webview altogether by omitting DHCP Option 3 (Default Gateway) from the configuration parameters distributed by dnsmasq23. Under RFC 2132, DHCP Option 3 advertises the IPv4 addresses of routers situated on the client's local subnet28. When this option is excluded, the client’s operating system assigns an IP address and netmask but installs no default gateway (0.0.0.0/0) targeting the Wi-Fi interface27.

Because the mobile operating system recognizes that the local wireless link possesses no external routing capacity, it categorizes the connection as an unrouted local peripheral, similar to a direct link with an enterprise camera, medical instrument, or wireless print server3. Under these conditions, the network stack skips captive portal probe routines entirely, avoiding the launch of the sandboxed Captive Network Assistant3. Outbound cellular internet routing remains fully operational for messaging and background tasks, while any packets destined for the local subnet are routed directly over the Wi-Fi link3. The user accesses the web application using a standard browser such as Safari or Chrome, retaining complete access to native download delegates and the device filesystem10.

### **Mechanism of Strategy B: The Apple CNA Breakout State Machine**

Strategy B leverages captive portal detection to automatically trigger the display of a landing page, but subsequently forces the operating system to dismiss the restricted modal1. When the device associates, the server hijacks the initial probe request, serving a custom landing page within the Captive Network Assistant1. When the user taps a button labeled "Access Artwork," client-side JavaScript issues an asynchronous request recording the client's MAC address on the server as authenticated, followed by a programmatic reload directive15.

This page refresh prompts the operating system’s background networking daemon to dispatch a follow-up probe to captive.apple.com1. Because the server now identifies the client as authorized, it responds with the exact string \<HTML\>\<HEAD\>\<TITLE\>Success\</TITLE\>\</HEAD\>\<BODY\>Success\</BODY\>\</HTML\>8. Upon parsing this payload, the operating system changes the modal navigation button from "Cancel" to "Done"1. When the user taps "Done," the sandboxed modal closes without disconnecting the Wi-Fi interface1. While functional, this design requires multi-stage user actions and remains susceptible to modifications in Apple's WISPr parsing heuristics, making Strategy A significantly more reliable for media distribution1.

## **Multi-Homed Routing and Address Resolution Integrity**

When a smartphone is linked concurrently to an offline local access point and a wide-area cellular carrier, its internal socket layer evaluates connection attempts using strict multi-homed routing metrics3. Deploying an offline server under these conditions requires addressing potential domain resolution failures3.

If the system architect attempts to use a local domain name or an arbitrary alias (such as http://freeart.local or http://gallery.lan), address resolution frequently fails on modern iOS and Android versions3. When the operating system detects that the local Wi-Fi interface lacks an external internet gateway, services like Apple’s Wi-Fi Assist and Android’s network evaluator mark the interface as untrusted for global queries3.

DNS queries for unfamiliar domains are automatically diverted to the cellular interface’s upstream name servers3. The mobile carrier’s DNS servers query public root authoritative zones, find no corresponding delegation, and return an unmapped NXDOMAIN response3. Furthermore, client-side implementations of DNS-over-HTTPS (DoH) and Apple’s iCloud Private Relay systematically bypass local DNS forwarders, nullifying wildcard interception rules (address=/\#/...) configured on the local access point13.

Navigating directly to a raw IPv4 literal address (such as http://192.168.4.1/art) circumvents the client-side DNS subsystem entirely26. When an application initiates a TCP connection toward a literal numeric IP, the host operating system checks its local routing table without issuing a DNS query17. Because the client’s Wi-Fi network interface is configured within the 192.168.4.0/24 subnet, the destination address matches the local interface prefix directly3. The kernel transmits the initial TCP SYN packet directly across the wireless medium via ARP to the Raspberry Pi's MAC address, maintaining connectivity regardless of cellular metrics, Private Relay states, or carrier DNS configurations3.

## **Infrastructure Configuration on Raspberry Pi OS**

Modern releases of Raspberry Pi OS (Debian 12 Bookworm and subsequent distributions) use NetworkManager as the primary networking engine, replacing legacy dhcpcd configurations6. A production-grade deployment cleanly isolates the physical layer access point, the DHCP assignment daemon, and the HTTP file-serving tier7.

### **NetworkManager Access Point Creation**

The onboard wireless controller (wlan0) is placed into access point mode using the nmcli command-line utility6. To prevent NetworkManager’s internal shared module from assigning conflicting gateway configurations, IPv4 assignment is set to manual7.

&nbsp;

&nbsp;

&nbsp;

Bash

\# Set regulatory wireless domain  
sudo raspi-config nonint do\_wifi\_country US

\# Provision the access point profile with WPA2-Personal security  
sudo nmcli con add type wifi ifname wlan0 mode ap con-name ArtAccessPoint ssid "FreeArtNetwork"  
sudo nmcli con modify ArtAccessPoint 802-11-wireless.band bg  
sudo nmcli con modify ArtAccessPoint 802-11-wireless.channel 6  
sudo nmcli con modify ArtAccessPoint wifi-sec.key-mgmt wpa-psk  
sudo nmcli con modify ArtAccessPoint wifi-sec.psk "ArtInstallation2025"  
sudo nmcli con modify ArtAccessPoint ipv4.method manual ipv4.addresses 192.168.4.1/24  
sudo nmcli con modify ArtAccessPoint ipv6.method disabled

\# Initialize the wireless broadcast  
sudo nmcli con up ArtAccessPoint

### **DHCP and DNS Orchestration via dnsmasq**

The dnsmasq service is installed to handle dynamic IP allocation while enforcing the omission of the default router23.

&nbsp;

&nbsp;

&nbsp;

Bash

sudo apt update && sudo apt install \-y dnsmasq

The configuration is isolated in a standalone drop-in file within /etc/dnsmasq.d/:

&nbsp;

&nbsp;

&nbsp;

Ini, TOML

\# /etc/dnsmasq.d/artserver.conf  
interface\=wlan0  
bind-interfaces  
dhcp-range\=192.168.4.10,192.168.4.200,255.255.255.0,2h

\# Omit DHCP Option 3 to suppress the distribution of a default gateway route  
dhcp-option\=3

\# Direct DNS queries back to the host interface  
dhcp-option\=6,192.168.4.1

\# Resolve any residual queries to the local host  
address\=/\#/192.168.4.1

&nbsp;

&nbsp;

&nbsp;

Bash

sudo systemctl restart dnsmasq

### **HTTP Server Optimization and Probe Emulation via NGINX**

The web tier is managed by NGINX, which combines efficient static media delivery with fallback locations capable of spoofing standard vendor probes if client devices issue them25.

&nbsp;

&nbsp;

&nbsp;

Bash

sudo apt install \-y nginx

The site configuration is deployed at /etc/nginx/sites-available/artserver:

&nbsp;

&nbsp;

&nbsp;

Nginx

server {  
    listen 80 default\_server;  
    listen \[::\]:80 default\_server;  
    server\_name \_;

    root /var/www/artserver;  
    index index.html;

    \# Emulated Apple captive portal responses  
    location \= /hotspot-detect.html {  
        default\_type text/html;  
        return 200 '\<HTML\>\<HEAD\>\<TITLE\>Success\</TITLE\>\</HEAD\>\<BODY\>Success\</BODY\>\</HTML\>';  
    }  
    location \= /library/test/success.html {  
        default\_type text/html;  
        return 200 '\<HTML\>\<HEAD\>\<TITLE\>Success\</TITLE\>\</HEAD\>\<BODY\>Success\</BODY\>\</HTML\>';  
    }

    \# Emulated Android connectivity probe response  
    location \= /generate\_204 {  
        return 204;  
    }  
    location \= /gen\_204 {  
        return 204;  
    }

    \# Emulated Microsoft NCSI probe response  
    location \= /connecttest.txt {  
        default\_type text/plain;  
        return 200 'Microsoft Connect Test';  
    }

    \# Primary web application interface  
    location / {  
        try\_files $uri $uri/ /index.html;  
    }

    \# Kernel-accelerated static media delivery  
    location /art/media/ {  
        alias /var/www/artserver/media/;  
        sendfile on;  
        tcp\_nopush on;  
        tcp\_nodelay on;  
        add\_header Cache-Control "public, max-age=31536000, immutable";  
    }  
}

&nbsp;

&nbsp;

&nbsp;

Bash

sudo ln \-sf /etc/nginx/sites-available/artserver /etc/nginx/sites-enabled/  
sudo rm \-f /etc/nginx/sites-enabled/default  
sudo systemctl restart nginx

## **Client-Side Presentation and Media Delivery Pipelines**

Delivering large image files across diverse mobile browsers requires handling differences in how mobile platforms store media assets5. On desktop browsers, an anchor element configured with a download attribute (\<a href="..." download\>) automatically saves the target asset into the system download directory. On mobile devices, however, this pattern often introduces user confusion or friction5.

&nbsp;

| Media Delivery Approach | Client Operating System | Resulting Storage Destination | User Experience Considerations |
| :---- | :---- | :---- | :---- |
| **Forced Download (\<a download\>)** | **iOS (Safari)** | Local Files.app (\~/Downloads) | Saves raw binary; does not automatically add asset to the Photos library5 |
| **Forced Download (\<a download\>)** | **Android (Chrome)** | Device filesystem (/Download) | Triggers download notification; file appears in generic downloads list |
| **Inline View (\<img src="..."\>)** | **iOS (Safari)** | Camera Roll (Photos.app) via long-press5 | Requires user awareness to long-press and select "Save to Photos"5 |
| **Inline View (\<img src="..."\>)** | **Android (Chrome)** | Photo Gallery via long-press | Presents contextual modal; user taps "Download image" to save to gallery |
| **Web Share API (navigator.share)** | **Cross-Platform** | Native System Share Sheet | Allows direct export into messaging, cloud storage, or local photo libraries |

On iOS, saving an image via a standard binary download places the asset into the local sandboxed Files application rather than the user's primary Photos library5. For general users seeking digital imagery, locating an asset inside Files.app can prove counterintuitive5.

To provide a consistent experience across all technical proficiencies, the front-end application at http://192.168.4.1/art should adopt a progressive approach:

* An initial responsive interface serves a lightweight, compressed thumbnail preview to minimize initial page-load latency over the local wireless link.  
* For saving the image, the interface presents two distinct actions:  
  * A primary button initiating a traditional binary download via an anchor tag marked with the download attribute and backed by an HTTP Content-Disposition: attachment response header, directing the file to the browser's download manager.  
  * A secondary action directing the browser to an unstyled, standalone viewing endpoint (/art/media/highres.jpg) served with Content-Disposition: inline. This opens the full-resolution asset directly within the native browser window, accompanied by instructional microcopy: *"Touch and hold the image, then tap 'Save to Photos'"*5. This enables native integration into the iOS Camera Roll and Android Gallery without relying on unsupported third-party browser plugins5.

## **End-to-End Operational Workflow**

The integrated operation of the offline art server coordinates every stage of the user interaction:

> 1. **WLAN Association:** The user joins the WPA2-secured network by scanning a physical QR code encoding the network credentials: WIFI:S:FreeArtNetwork;T:WPA;P:ArtInstallation2025;;6. This eliminates manual passphrase entry while preventing unauthorized ambient connections from saturating the access point6.  
> 2. **Peripheral Network Negotiation:** The device connects and requests network parameters via DHCP1. The dnsmasq service assigns an IP address within 192.168.4.0/24 but explicitly omits DHCP Option 323. Detecting no default gateway, the mobile operating system treats the Wi-Fi connection as a non-routing local peripheral3. Captive portal detection is skipped entirely, keeping background cellular connectivity active and preventing the restricted Captive Network Assistant from launching3.  
> 3. **Application Access:** Signage at the installation directs the user to open Safari or Chrome and navigate to http://192.168.4.1/art, or to scan a second QR code encoding that exact URL26. Because the request targets a raw IPv4 literal on the local subnet, the operating system routes the traffic directly over the Wi-Fi interface, bypassing cellular DNS servers and avoiding name resolution failures3.  
> 4. **Media Transmission:** The NGINX web server delivers the lightweight preview page, followed by the high-resolution JPEG via kernel-level sendfile acceleration25. The user chooses between saving the raw binary file to the device's downloads folder or opening the full-resolution asset inline to save it directly to their primary photo library via the native long-press gesture5.

#### **Works cited**

> 1. Captive Network Portal Behavior \- Wireless Broadband Alliance, [https://captivebehavior.wballiance.com/](https://captivebehavior.wballiance.com/)  
> 2. Captive Portal Detection: How It Works \- Cloud4Wi, [https://cloud4wi.ai/blog/captive-portal-detection/](https://cloud4wi.ai/blog/captive-portal-detection/)  
> 3. How to Use Mobile Data When Connected to Wi-Fi, [https://help.snappic.com/en/articles/7872816-how-to-use-mobile-data-when-connected-to-wi-fi](https://help.snappic.com/en/articles/7872816-how-to-use-mobile-data-when-connected-to-wi-fi)  
> 4. What Is Apple Captive Network Assistant? \- SecureW2, [https://securew2.com/blog/apple-captive-network-assistant-understanding-captive-portals-on-ios-devices](https://securew2.com/blog/apple-captive-network-assistant-understanding-captive-portals-on-ios-devices)  
> 5. New wallpaper designs available\! \- MOOMIN SHOP, [https://shop.moomin.co.jp/en/blogs/fanclub/%E5%A3%81%E7%B4%99%E3%81%AB%E6%96%B0%E3%83%87%E3%82%B6%E3%82%A4%E3%83%B3%E3%81%8C%E7%99%BB%E5%A0%B4-2](https://shop.moomin.co.jp/en/blogs/fanclub/%E5%A3%81%E7%B4%99%E3%81%AB%E6%96%B0%E3%83%87%E3%82%B6%E3%82%A4%E3%83%B3%E3%81%8C%E7%99%BB%E5%A0%B4-2)  
> 6. Raspberry Pi Wireless Access Point \- Pi My Life Up, [https://pimylifeup.com/raspberry-pi-wireless-access-point/](https://pimylifeup.com/raspberry-pi-wireless-access-point/)  
> 7. Configuring a WiFi Access Point using Raspberry Pi..... How To with, [https://community.element14.com/products/raspberry-pi/f/forum/53996/configuring-a-wifi-access-point-using-raspberry-pi-how-to-with-the-new-kernel](https://community.element14.com/products/raspberry-pi/f/forum/53996/configuring-a-wifi-access-point-using-raspberry-pi-how-to-with-the-new-kernel)  
> 8. How Does Apple CNA works for Guest Users connecting to Aruba, [https://community.instant-on.hpe.com/communities/community-home/digestviewer/viewthread?MID=487](https://community.instant-on.hpe.com/communities/community-home/digestviewer/viewthread?MID=487)  
> 9. Turn Your Raspberry Pi into an Access Point (Bookworm Ready), [https://raspberrytips.com/access-point-setup-raspberry-pi/](https://raspberrytips.com/access-point-setup-raspberry-pi/)  
> 10. How to Open the Captive Portal Login Page (When It Won't Show Up), [https://www.captivewifi.com/blog/how-to-open-captive-portal-login-page](https://www.captivewifi.com/blog/how-to-open-captive-portal-login-page)  
> 11. How to force a public Wi-Fi network login page to open \- Zapier, [https://zapier.com/blog/open-wifi-login-page/](https://zapier.com/blog/open-wifi-login-page/)  
> 12. An undocumented change to Captive Network Assistant settings in, [https://grahamrpugh.com/2014/10/29/undocumented-change-to-captive-network-assistant-settings-in-yosemite.html](https://grahamrpugh.com/2014/10/29/undocumented-change-to-captive-network-assistant-settings-in-yosemite.html)  
> 13. Why Apple, Google & Samsung are replacing captive portals, [https://www.purple.ai/en-gb/blogs/the-death-of-the-captive-portal-why-apple-google-and-samsung-are-killing-legacy-wifi](https://www.purple.ai/en-gb/blogs/the-death-of-the-captive-portal-why-apple-google-and-samsung-are-killing-legacy-wifi)  
> 14. Just how limited is the Captive Network Assistant?, [https://discussions.apple.com/thread/5258403](https://discussions.apple.com/thread/5258403)  
> 15. Captive Wifi Popup: Click a link to open Safari \- Stack Overflow, [https://stackoverflow.com/questions/23281552/captive-wifi-popup-click-a-link-to-open-safari](https://stackoverflow.com/questions/23281552/captive-wifi-popup-click-a-link-to-open-safari)  
> 16. Handing out NO gateway via DHCP? · Issue \#1372 \- GitHub, [https://github.com/espressif/arduino-esp32/issues/1372](https://github.com/espressif/arduino-esp32/issues/1372)  
> 17. Should an Access Point without internet set the DHCP Offer-Router, [https://networkengineering.stackexchange.com/questions/52156/should-an-access-point-without-internet-set-the-dhcp-offer-router-3-option](https://networkengineering.stackexchange.com/questions/52156/should-an-access-point-without-internet-set-the-dhcp-offer-router-3-option)  
> 18. Implement RFC 8908 Captive Portal API · Issue \#7040 \- GitHub, [https://github.com/inverse-inc/packetfence/issues/7040](https://github.com/inverse-inc/packetfence/issues/7040)  
> 19. RFC 8908: Captive Portal API, [https://www.rfc-editor.org/info/rfc8908/](https://www.rfc-editor.org/info/rfc8908/)  
> 20. How to modernize your captive network \- Discover \- Apple Developer, [https://developer.apple.com/news/?id=q78sq5rv](https://developer.apple.com/news/?id=q78sq5rv)  
> 21. ISE 2.2+ Apple CNA (Captive Network Assistant) Mini-Browser for, [https://community.cisco.com/t5/security-knowledge-base/ise-2-2-apple-cna-captive-network-assistant-mini-browser-for/ta-p/3636988](https://community.cisco.com/t5/security-knowledge-base/ise-2-2-apple-cna-captive-network-assistant-mini-browser-for/ta-p/3636988)  
> 22. Accessing WiFi LAN (no gateway) and mobile network simultaneously, [https://stackoverflow.com/questions/28403627/accessing-wifi-lan-no-gateway-and-mobile-network-simultaneously](https://stackoverflow.com/questions/28403627/accessing-wifi-lan-no-gateway-and-mobile-network-simultaneously)  
> 23. Pi AP for LAN, but use iPhone cellular for internet gateway, [https://forums.raspberrypi.com/viewtopic.php?t=202271](https://forums.raspberrypi.com/viewtopic.php?t=202271)  
> 24. Python flask captive portal \- Server Fault, [https://serverfault.com/questions/1108718/python-flask-captive-portal](https://serverfault.com/questions/1108718/python-flask-captive-portal)  
> 25. Captive Portal w/ nginx, hostapd, nftables, dnsmasq, [https://unix.stackexchange.com/questions/792009/captive-portal-w-nginx-hostapd-nftables-dnsmasq](https://unix.stackexchange.com/questions/792009/captive-portal-w-nginx-hostapd-nftables-dnsmasq)  
> 26. Force captive portal sign in page to open \- Apple Community, [https://discussions.apple.com/thread/250195103](https://discussions.apple.com/thread/250195103)  
> 27. Notes on the Raspberry Pi Zero W \+ Emacs \+ iPad, [https://forum.zettelkasten.de/discussion/2269/notes-on-the-raspberry-pi-zero-w-emacs-ipad](https://forum.zettelkasten.de/discussion/2269/notes-on-the-raspberry-pi-zero-w-emacs-ipad)  
> 28. How to configure DHCP Option 3 (Routers) to assign Multiple, [https://www.sonicwall.com/support/knowledge-base/how-to-configure-dhcp-option-3-routers-to-assign-multiple-gateway-ip-addresses/kA1VN0000000FNM0A2](https://www.sonicwall.com/support/knowledge-base/how-to-configure-dhcp-option-3-routers-to-assign-multiple-gateway-ip-addresses/kA1VN0000000FNM0A2)  
> 29. I don't want my DHCP to be a default gateway \- Super User, [https://superuser.com/questions/306121/i-dont-want-my-dhcp-to-be-a-default-gateway](https://superuser.com/questions/306121/i-dont-want-my-dhcp-to-be-a-default-gateway)  
> 30. After pfsense boots, DHCP offer does not contain \`Option: (3) Router, [https://forum.netgate.com/topic/182735/after-pfsense-boots-dhcp-offer-does-not-contain-option-3-router-so-client-has-no-default-route](https://forum.netgate.com/topic/182735/after-pfsense-boots-dhcp-offer-does-not-contain-option-3-router-so-client-has-no-default-route)  
> 31. Wifi „no internet connection“ issues (iOS16), [https://discussions.apple.com/thread/254213720](https://discussions.apple.com/thread/254213720)  
> 32. Captive Portal Issues \- a workaround that worked for me. : r/MacOS, [https://www.reddit.com/r/MacOS/comments/1ay7exb/captive\_portal\_issues\_a\_workaround\_that\_worked/](https://www.reddit.com/r/MacOS/comments/1ay7exb/captive_portal_issues_a_workaround_that_worked/)  
> 33. Configuration \- Raspberry Pi Documentation, [https://www.raspberrypi.com/documentation/computers/configuration.html](https://www.raspberrypi.com/documentation/computers/configuration.html)  
> 34. How To Start a Fake Access Point (Fake WIFI) \- zSecurity, [https://zsecurity.org/how-to-start-a-fake-access-point-fake-wifi/](https://zsecurity.org/how-to-start-a-fake-access-point-fake-wifi/)  
> 35. Trixie NetworkManager vs systemd-networkd advice, [https://forums.raspberrypi.com/viewtopic.php?t=396162](https://forums.raspberrypi.com/viewtopic.php?t=396162)  
> 36. Routing with two Gateway (Page 2\) / Networking, Server, and, [https://bbs.archlinux.org/viewtopic.php?id=269752\&p=2](https://bbs.archlinux.org/viewtopic.php?id=269752&p=2)  
> 37. Wireless — Omid Raha MyStack 0.1 documentation, [https://omidraha.com/en/latest/src/linux/wireless.html](https://omidraha.com/en/latest/src/linux/wireless.html)  
> 38. Captive portal using NGINX, hostapd, create\_ap and dnsmasq in, [https://github.com/orlandev/CaptivePortal](https://github.com/orlandev/CaptivePortal)  
> 39. Frequently Asked Questions \- LiveWall, [https://livewall.no/faq](https://livewall.no/faq)
