#pragma once

// Advertise this blimp as <hostname>.local and as a _blimp._udp service, so a
// basestation can find it without reading the DHCP address from the console.
// Safe to call before the station has an address; the responder starts
// answering once the interface comes up.
void blimp_mdns_start(void);
