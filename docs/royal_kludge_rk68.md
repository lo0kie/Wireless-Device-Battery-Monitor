# Royal Kludge RK68 keyboard: why there is no battery driver (yet)

Checked on Windows 11 on 2026-11-25 with an RK68 connected two ways: through its `25A7:FA70`
2.4 GHz receiver, and through the USB-C cable. The conclusion is negative: none of the device ids
this keyboard exposes carries a readable battery level, so no driver was added. What follows is the
evidence, plus the frame layout of RK's own web driver, which is the part worth keeping.

## What the keyboard exposes

| Path | VID:PID | Collections | Notes |
|---|---|---|---|
| 2.4 GHz receiver | `25A7:FA70` | `0001:0006` x4 (keyboard), `0001:0002`, `0001:0080`, `000C:0001`, `FF01:0000` (8 byte in), `FF02:0002` (8 byte feature) | Areson/CompX bridge, product string "2.4G Wireless Receiver" |
| USB cable | `258A:0052` | `FF00:0001` (1025 byte feature), 6-byte command collection, keyboard/consumer/system collections | Product string "Gaming KB", Sinowealth SH68F90A |
| USB cable (second node) | `3537:2106` | `FF7A:0001`, 65 byte in + 65 byte out, no feature report | Product string "HID zkm" |

`048D:C197` ("ITE Device(8258)", keyboard collection plus `FF89:*` RGB channels) is **not** part of
this keyboard: it is enumerated whether or not the keyboard is plugged in, so it is another device
on the same hub and was left alone.

## Channels that were ruled out

All of these are read-only probes; nothing was configured or stored.

1. **Report-descriptor scan.** `HidP_GetValueCaps` over every collection of `25A7:FA70`, `258A:0052`,
   `3537:2106` and `048D:C197`: there is no `0x06:0x20` (Battery Strength), no `0x85` (Battery
   System) and no `0x84` (Power Device) usage anywhere. A standard HID battery field is therefore not
   what this keyboard uses, so neither the generic HID fallback nor the Bluetooth property path can
   ever see it.
2. **`25A7:FA70` / `FF02:0002` (8-byte feature).** Only report id `06` exists. A full `0x00..0xFF`
   sweep of the command byte produced nothing but an echo, with `byte2` equal to
   `cmd & 0x80 ? 1 : 2` (a direction flag). `byte6..7` stay `D8 AE`. The report is a status
   mailbox, not a command channel. `byte2` does follow the link state: it dropped to `01` while the
   keyboard was unlinked and returned to `02` once the keyboard was back.
3. **`25A7:FA70` / `FF01:0000` (8-byte input).** Zero frames while idle and zero frames while keys
   were being typed. `HidD_GetInputReport` on report ids `0..255` fails for all of them.
4. **`258A:0052` / `FF00:0001` (1025-byte feature).** Report id `09` returns a 1024-byte window of
   MCU memory (USB strings such as "KB  SINO WEALTH" and 8051 code). It is Sinowealth's ISP channel,
   i.e. the firmware download port. **Do not write to it.** The 6-byte collection next to it only
   sets the window address/block.
5. **`3537:2106` / `FF7A:0001` (65+65).** Two documented read-only queries were sent and neither
   got a reply:
   - the QiWang `GET_BATTER_VOL` frame already used for the RK MG6 Pro (`00 01 01 02 01`), see
     `src/drivers/royal_kludge.cpp`;
   - RK's BeiYing battery frame, layout below.

## RK's web driver: the BeiYing frame layout

`drive.rkgaming.com` (a WebHID driver, nothing to install) ships its device table and protocol in
`assets/index-BUnhtJhF.js`. For the RK 68 the table has two entries:

| Mode | VID:PID | Usage page:usage | protocolType |
|---|---|---|---|
| USB | `258A:02AC` | `FF00:0001` | BeiYing |
| Dongle | `3554:FA09` | `FF02:0002` | BeiYing |

Note that neither id matches the unit tested here (`25A7:FA70` over 2.4 GHz, `258A:0052` over the
cable), which is why RK's own driver does not claim it either.

The frame is 63 bytes, written as a feature report and read back from the same report id:

```
[0]  crc   = sum(frame[1..61]) & 0xFF
[1]  0x50  fixed
[2]  payload length
[3]  sn
[4]  cmdId
[5]  dataOffset
[6..] payload
```

`getBattery()` for the USB path sets `sn = 2`, `dataOffset = 0x81`, `cmdId = 0x4F`, an empty
payload, sends the frame as feature report `3`, sleeps 200 ms and calls `getFeature(3)`. The reply
is parsed by an `m5i` reader: `[1]` fixed value `0x50`, `[2]` cmdRsp, `[3]` dataLength, `[4]` sn,
`[5]` offset, payload from `[6]`.

For the dongle path the same driver uses a different frame class: `cmdId = 0x3A`, `sn = 0x50`,
a 1-byte payload, feature report `9`.

Other ids in the same table (`0F62:1000`, `0F62:1001`, `0F62:100C`, `0F62:100D`, and `FFFF:1000`)
are `QiWang` instead, and newer SKUs run that QiWang traffic through an encrypted SDK worker
(`setDeviceDataEncryptType(2)`), so a plaintext frame is ignored there.

## Why no code was added

A driver can only be written against a channel that answers. None of the ids of the tested unit
answers a battery query, and the only writable channel that does react (`258A:0052` report `09`) is
the firmware port, where a wrong write bricks the keyboard. Reporting a made-up or fixed percentage
would be worse than reporting nothing, so the keyboard is left unclaimed and the app simply does not
list it.

## What would unblock it

- Pair the keyboard over Bluetooth: the app already reads the Windows battery property, so the
  percentage shows up without any code change if Windows exposes it.
- Or capture the RK web driver in Chrome (DevTools -> the WebHID `sendFeatureReport` /
  `receiveFeatureReport` calls) with the keyboard in the mode to be supported. That gives the exact
  report id, `cmdId` and payload length for this specific unit, which is what the BeiYing decoder
  above expects.
- Or repeat this work on a unit whose ids are in RK's table (`258A:02AC`, `3554:FA09`); the frame
  layout above should be enough to write the driver.
