# Connect an Android phone to the Studio pendant

KerfKite Pocket sends pendant commands to the **Studio machine server**. The server owns the CNC controller connection, machining preview and job. The phone needs an IP route to the server's **phone monitor** port, which defaults to 8766. The same native pendant works over Wi-Fi, a USB tether network or a Bluetooth Personal Area Network (PAN). Your phone, server operating system and network drivers determine which links are available.

## Choose the network

| Link | Set up the connection | Address to use in the pairing link |
| --- | --- | --- |
| Wi-Fi | Connect the phone and Studio host to the same private LAN. A wired Studio host on the same LAN also works. Guest network isolation can block communication between devices. | The host's Wi-Fi or Ethernet address on that LAN. |
| USB tether | Plug the phone into the computer running Studio using a USB data cable. In Android Settings, search for **USB tethering** and enable it. On a compatible Windows or Linux host, wait for the USB network adapter to acquire an address. The controller can remain on another port on the host. Google lists native Android USB tethering as unsupported on Mac. | The host's address on its USB network adapter. |
| Bluetooth PAN | Pair the phone and Studio host in their operating systems. In Android Settings, search for **Bluetooth tethering** and enable it. On a PAN-capable host, connect to the phone's network access service. Pairing alone does not establish the network. | The host's address on its Bluetooth network adapter. |

Use the **server's** interface address. The phone's gateway address, the controller's serial device and a Bluetooth device address cannot identify the listening server. On Linux, including a Pi, `ip -brief address` lists interfaces and their IPv4 addresses. On Windows use `ipconfig`. For Wi-Fi on macOS see **System Settings → Network → Wi-Fi → Details → TCP/IP**. Carrier or device policies can affect tethering availability. [Google Android tethering help](https://support.google.com/android/answer/9059108)

## Pair and use the pendant

1. In the local machine workstation open **Control → Phone pendant**. Choose **Enable phone pairing**, enter the server address for the link you selected and copy the generated private pairing URL. The workstation can also start with `python -m studio.server --monitor`. Its terminal displays the port and a token. Replace the host placeholder with the server address.
2. Open **CNC pendant** in Pocket. Paste the complete pairing URL and tap **Pair**. The token grants workshop controls to its holder. Keep the link private.

   **Encrypted by default.** When the workstation has the `cryptography` package (Studio's `web` extra), enabling pairing creates a certificate for that pairing and puts its SHA-256 fingerprint in the link after `&fp=`. Pocket 0.5.0 or later connects over HTTPS and accepts only that certificate; it does not rely on a certificate authority or on the address in the link. A new certificate is made each time pairing is enabled, and disabling pairing discards it.

   **Older phones and browsers.** Pocket 0.4.1 and earlier cannot read a link that carries a fingerprint, and a phone browser warns about the certificate. For those, tick **Unencrypted pairing** before **Enable phone pairing**, or start the server with `--monitor-plaintext`. Plain HTTP sends the token and every command across the link in the clear, and Pocket asks for confirmation on a trusted private network. A workstation without the `cryptography` package, including the current desktop installers, also uses HTTP and says so beside the link.

   **Your own certificate.** `--monitor-cert` and `--monitor-key` still serve a certificate you supply; it must be valid for the host in the link and trusted by the phone.
3. Confirm that the pendant shows fresh controller status. In simulator mode it identifies the **Simulator**. Jog sends one selected increment on one axis. Home and Zero XYZ require confirmation. Prepare, preview and arm a job at the workstation before starting it from Pocket. The server consumes that arm on the first start attempt. Pause, Resume and Stop use the connected controller's supported job controls.

Leave the workstation tab that armed the preview open and connected. On each phone Start, the server gives that tab 1.8 seconds to confirm that its current preview still matches the arm. Editing the design, closing the tab or losing its response prevents that Start. On a timeout, check controller status, generate a fresh preview and arm again. The workstation shows a pending disarm if its connection drops during an edit.

Stop works while a Start awaits confirmation. It cancels the arm and invalidates earlier phone commands that arrive late. Pause, Resume and Stop for a running job work through the server even when the workstation tab is closed. If the phone asks for an update, update Pocket or reload the browser monitor and get fresh controller status before issuing another movement. A paired older client can still read status and send Stop.

4. **Forget pairing** clears the phone session. **Disable and revoke pairing** on the server ends access for all paired clients. For another network link, use the server's address on that link and pair again.

If pairing times out, open the monitor base URL in the phone browser on the same link. Getting the monitor page confirms reachability. If it cannot load, check the server interface address, monitor port, host firewall for this private subnet (on Windows the inbound rule must cover the network profile the adapter is actually using, and third-party security software may apply its own), Wi-Fi client isolation or whether the tether/PAN adapter is active. An expired token needs a fresh pairing link. If the connection drops during a job, check the controller at the workstation before taking further action. A network disconnect does not establish that motion stopped.

## Emulator acceptance

The native test uses a disposable Android emulator, an authenticated loopback monitor and the simulated GRBL controller. Select `--route lan` to reach Studio through the emulator’s virtual LAN, or `--route reverse` to forward the connection with ADB. Both routes reach a local Studio receiver. From the repository root, with a disposable emulator already booted and platform-tools on PATH, run

```sh
python validation/android_pendant_workflow.py --serial emulator-5554 --route lan --apk path/to/KerfKite-Pocket-Android.apk
```

The harness installs the supplied APK and operates the native pendant. With `--route reverse` it also creates and removes an emulator port mapping. USB debugging and ADB are developer tools for this test. Normal pendant operation uses one of the network links above.

Pocket 0.4.1 was exercised with Studio 0.6.1 on an Android API 37 emulator with 2 GB of RAM. The paired pendant reached the simulator over the virtual LAN and through ADB port forwarding. [Studio 0.6.1 verification record](https://github.com/ShaunPrice/KerfKite/blob/v0.6.1/validation/performance-061.json)

In an earlier upgrade test, Pocket 0.3.0 replaced Pocket 0.2.1 on an Android API 37 emulator. Over the virtual LAN, the native screen paired and sent a 0.1 mm X jog, Home, Zero XYZ, Start, Pause, Resume and Stop to the simulator. Each action reached Studio once. The test also checked that Stop disarmed the job and that Forget cleared pairing. Physical Android handsets, Wi-Fi, USB tether drivers, Bluetooth PAN links and machine motion await equipment testing.
