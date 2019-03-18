#### Hardware Platform
* nRF51-DK PCA10028 

*<!> nRF52 demo project is in this branch [**nordic**](https://github.com/MiEcosystem/mijia_ble_secure/tree/nordic)*
#### Requirement
- arm MDK 5.25
- SEGGER Jlink
- Git

#### How to use

1. download nRF5 SDK 12.3.0 [here](https://www.nordicsemi.com/Software-and-Tools/Software/nRF5-SDK/Download#infotabs)
2. $ cd SDK_12.3.0\examples\ble_peripheral\ directory.
3. $ git clone --recursive https://github.com/MiEcosystem/mijia_ble_secure.git -b nordic_legacy
3. change PRODUCT_ID to your product ID (i.e. pid), that you got when registered in [Mi IoT](https://iot.mi.com/index.html).

#### Diagnose

Make sure you have installed JLink.
```bash
$ JLinkExe -device NRF51422_XXAA -if swd -speed 8000 -RTTTelnetPort 2000
```
Then open a new terminal tab:
```bash
$ telnet 127.0.0.1 2000
```
The log infomation will be print in this telnet session.
