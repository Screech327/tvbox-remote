# Diagrams

GitHub draws these automatically. Solid lines are cables; dashed lines are network or Wi-Fi.

## Which layout do I need?

```mermaid
flowchart TD
    Q1{"Do you still want to watch<br/>on a TV too?"}
    Q1 -- "No, remote only" --> A["<b>Layout A</b><br/>Box → capture card → computer"]
    Q1 -- Yes --> Q2{"Is the TV in the same room<br/>as the TV box and computer?"}
    Q2 -- Yes --> B["<b>Layout B</b><br/>Box → splitter → TV + capture card"]
    Q2 -- "No, another room" --> C["<b>Layout C</b><br/>Box → splitter → capture card + HDMI extender → TV"]
    A --> R{"Watch away from home?"}
    B --> R
    C --> R
    R -- Yes --> T["Add <b>Tailscale</b><br/>(free, no port forwarding)"]
    R -- No --> L["Use it on your home network"]
```

## How it works (any layout)

```mermaid
flowchart LR
    subgraph TVBOX["TV box"]
        HDMIOUT["HDMI out"]
        USB["USB port"]
    end
    PICO["Pico 2 W<br/>acts as USB keyboard + mouse"]
    CAP["USB HDMI<br/>capture card"]
    subgraph HOST["Host computer (Linux)"]
        FF["ffmpeg<br/>encodes H.264"]
        MTX["MediaMTX<br/>WebRTC stream"]
        CTL["Control page<br/>server.py :80"]
    end
    YOU["Your browser<br/>(PC, laptop or phone)"]

    HDMIOUT -- HDMI --> CAP
    CAP -- USB --> FF --> MTX
    PICO -- "USB: key presses, mouse" --> USB
    YOU -. "watch (WebRTC)" .-> CTL
    CTL -. "proxies the video handshake" .-> MTX
    YOU -. "clicks, keys, buttons" .-> CTL
    CTL -. "Wi-Fi, UDP 4210 + token" .-> PICO
```

The browser only talks to the host. The host passes commands to the Pico with a token the browser never sees.

## Layout A: remote only (no TV)

```mermaid
flowchart LR
    BOX["TV box"] -- HDMI --> CAP["Capture card"] -- USB --> HOST["Host computer"]
    PICO["Pico 2 W"] -- USB --> BOX
    PICO -. Wi-Fi .- NET(("Home network")) -. Ethernet or Wi-Fi .- HOST
```

## Layout B: TV in the same room

```mermaid
flowchart LR
    BOX["TV box"] -- HDMI --> SPL["HDMI splitter<br/>(must work with<br/>capture cards)"]
    SPL -- "HDMI out 1" --> CAP["Capture card"] -- USB --> HOST["Host computer"]
    SPL -- "HDMI out 2" --> TV["TV"]
    PICO["Pico 2 W"] -- USB --> BOX
    PICO -. Wi-Fi .- NET(("Home network")) -. Ethernet or Wi-Fi .- HOST
```

## Layout C: TV in another room

```mermaid
flowchart LR
    subgraph RACK["Rack / closet"]
        BOX["TV box"] -- HDMI --> SPL["HDMI splitter<br/>(must work with<br/>capture cards)"]
        SPL -- "HDMI out 1" --> CAP["Capture card"] -- USB --> HOST["Host computer"]
        SPL -- "HDMI out 2" --> TX["HDMI extender<br/>sender"]
        PICO["Pico 2 W"] -- USB --> BOX
    end
    subgraph ROOM["Living room"]
        RX["HDMI extender<br/>receiver"] -- HDMI --> TV["TV"]
    end
    TX == "Cat5e/Cat6 run" ==> RX
    PICO -. Wi-Fi .- NET(("Home network")) -. Ethernet .- HOST
```

**Don't use the extender's loop-out port to feed the capture card.** It passes the copy protection (HDCP)
straight through, and the capture card shows black. The splitter goes first in the chain.

## Watching away from home (Tailscale)

```mermaid
flowchart LR
    PHONE["Phone / laptop<br/>anywhere"] -. "Tailscale (encrypted)" .-> HOST["Host computer<br/>Tailscale installed"]
    HOST -. "Wi-Fi, UDP 4210" .-> PICO["Pico 2 W"] -- USB --> BOX["TV box"]
    BOX -- HDMI --> CAP["Capture card"] -- USB --> HOST
```

No ports are opened on the router. **Never** port-forward the control page: anyone who can open it controls the
box.

## Reference build (the original home lab)

Overkill for most people, but it shows how it fits into a home lab. Layout C on a Proxmox server.

```mermaid
flowchart LR
    subgraph RACK["10-inch rack"]
        GW["UniFi Cloud Gateway Ultra<br/>router"]
        SW["UniFi Flex Mini 2.5G<br/>switch"]
        subgraph OMEN["Old laptop: Proxmox host"]
            CT["Linux container<br/>ffmpeg + MediaMTX + control page"]
        end
        BOX["vSeeBox<br/>(isolated IoT network)"]
        SPL["ezcoo SP12H2<br/>HDMI splitter"]
        CAPT["Elgato Cam Link 4K"]
        TX["OREI HDMI extender<br/>sender"]
        PICO["Pico 2 W"]
    end
    subgraph LIVING["Living room"]
        RX["OREI receiver"] -- HDMI --> TV["TV"]
    end
    GW --- SW
    SW -- Ethernet --- OMEN
    SW -- Ethernet --- BOX
    BOX -- HDMI --> SPL
    SPL -- HDMI --> CAPT -- "USB (passed into the container)" --> CT
    SPL -- HDMI --> TX
    TX == "Cat6 in-wall run" ==> RX
    PICO -- USB --> BOX
    PICO -. Wi-Fi .- GW
```
