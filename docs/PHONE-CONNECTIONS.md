# Connect a Galaxy phone to the Studio pendant

KerfKite Pocket sends pendant commands to the **Studio machine server**. The server owns the CNC controller connection, machining preview and job. The phone needs an IP route to the server's **phone monitor** port, which defaults to 8766. The same native pendant works over Wi-Fi, a USB tether network or a Bluetooth Personal Area Network (PAN). Your phone, server operating system and network drivers determine which links are available.

## Choose the network

| Link | Set up the connection | Address to use in the pairing link |
| --- | --- | --- |
| Wi-Fi | Connect the phone and Studio host to the same private LAN. A wired Studio host on the same LAN also works. Guest network isolation can block communication between devices. | The host's Wi-Fi or Ethernet address on that LAN. |
| USB tether | Plug the S23 into the computer running Studio using a USB data cable. On the phone open **Settings → Connections → Mobile Hotspot and Tethering → USB tethering**. On a compatible Windows or Linux host, wait for the USB network adapter to acquire an address. The controller can remain on another port on the host. Google lists native Android USB tethering as unsupported on Mac. | The host's address on its USB network adapter. |
| Bluetooth PAN | Pair the phone and Studio host in their operating systems. On the S23 enable **Settings → Connections → Mobile Hotspot and Tethering → Bluetooth tethering**. On a PAN-capable host, connect to the phone's network access service. Pairing alone does not establish the network. | The host's address on its Bluetooth network adapter. |

Use the **server's** interface address. The phone's gateway address, the controller's serial device and a Bluetooth device address cannot identify the listening server. On Linux, including a Pi, `ip -brief address` lists interfaces and their IPv4 addresses. On Windows use `ipconfig`. For Wi-Fi on macOS see **System Settings → Network → Wi-Fi → Details → TCP/IP**. Carrier or device policies can affect tethering availability. [Samsung connection options](https://www.samsung.com/us/support/answer/ANS10002546/) · [Google Android tethering help](https://support.google.com/android/answer/9059108)

## Pair and use the pendant

1. In the local machine workstation open **Control → Phone pendant**. Choose **Enable phone pairing**, enter the server address for the link you selected and copy the generated private pairing URL. The workstation can also start with `python -m studio.server --monitor`. Its terminal displays the port and a token. Replace the host placeholder with the server address.
2. Open **CNC pendant** in Pocket. Paste the complete pairing URL and tap **Pair**. For HTTPS use a certificate valid for that host and trusted by the app. Plain HTTP sends the token across the link and requires an explicit confirmation on a trusted private network. The token grants workshop controls to its holder. Keep it private.
3. Confirm that the pendant shows fresh controller status. In simulator mode it identifies the **Simulator**. Jog sends one selected increment on one axis. Home and Zero XYZ require confirmation. Prepare, preview and arm a job at the workstation before starting it from Pocket. The server consumes that arm on the first start attempt. Pause, Resume and Stop use the connected controller's supported job controls.
4. **Forget pairing** clears the phone session. **Disable and revoke pairing** on the server ends access for all paired clients. For another network link, use the server's address on that link and pair again.

If pairing times out, open the monitor base URL in the phone browser on the same link. Getting the monitor page confirms reachability. If it cannot load, check the server interface address, monitor port, host firewall for this private subnet, Wi-Fi client isolation or whether the tether/PAN adapter is active. An expired token needs a fresh pairing link. If the connection drops during a job, check the controller at the workstation before taking further action. A network disconnect does not establish that motion stopped.

## Emulator acceptance

The native test uses a disposable Android emulator, an authenticated loopback monitor and the simulated GRBL controller. Select `--route lan` to reach Studio through the emulator’s virtual LAN, or `--route reverse` to forward the connection with ADB. Both routes reach a local Studio receiver. From the repository root, with a disposable emulator already booted and platform-tools on PATH, run

```sh
python validation/android_pendant_workflow.py --serial emulator-5554 --route lan --apk path/to/KerfKite-Pocket-Android.apk
```

The harness installs the supplied APK and operates the native pendant. With `--route reverse` it also creates and removes an emulator port mapping. USB debugging and ADB are developer tools for this test. Normal pendant operation uses one of the network links above.

The signed 0.3.0 APK upgraded an installed signed 0.2.1 on an Android API 37 emulator. Over the virtual LAN, the native screen paired and sent a 0.1 mm X jog, Home, Zero XYZ, Start, Pause, Resume and Stop to the simulator. Each action reached Studio once. The test also checked that Stop disarmed the job and that Forget cleared pairing. Physical Galaxy hardware, Wi-Fi, USB tether drivers, Bluetooth PAN links and machine motion await equipment testing.
