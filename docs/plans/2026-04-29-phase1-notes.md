# Phase 1 discovery — firmware audio path (2026-04-29)

Sources: `~/esp32-s3-box-3/main/application.cc`, `main/audio/audio_service.cc`, `main/protocols/websocket_protocol.cc`.

## Wake word → uplink

1. **`AudioService::EncodeWakeWord()`** / **`PopWakeWordPacket()`** (`audio_service.cc`): after detection, `wake_word_->EncodeWakeWordData()` runs; encoded wake audio is read as **binary Opus payloads** in **`AudioStreamPacket::payload`** (`GetWakeWordOpus`).
2. **`Application::ContinueWakeWordInvoke`** (`application.cc`, ~827+): if **`CONFIG_SEND_WAKE_WORD_DATA`** is enabled, **all** wake-word packets are sent with **`protocol_->SendAudio(std::move(packet))`** in a loop; then **`protocol_->SendWakeWordDetected(wake_word)`** notifies the server. **Server hook (Layer A)** should consume this early Opus/PCM window before general STT.
3. **Post-wake utterance**: **`SendStartListening`** is issued when entering listening state; uplink audio thereafter uses the normal send queue (Opus framed per `WebsocketProtocol::SendAudio`).

## Transport

- **`WebsocketProtocol`** (`websocket_protocol.cc`): JSON hello includes `"format": "opus"` for the audio channel. Binary frames carry encoded audio; **`SendText`** used for control messages.

## Decisions

| Question | Finding |
|----------|---------|
| Wake buffer reaches server? | **Yes**, when `CONFIG_SEND_WAKE_WORD_DATA` is on — wake Opus packets are explicitly sent before `SendWakeWordDetected`. |
| Gate insertion (firmware vs server)? | **Server-side** per plan: deserialize Opus → PCM (or use server’s existing decode path), then ECAPA / diarization before STT dispatch. |
| Contiguous with utterance? | Wake packets are sent **first** in sequence, then listening stream follows after `SendStartListening` / mode change — treat as **prepended stream** for server pipeline design. |

## Next steps (implementation)

- Confirm `CONFIG_SEND_WAKE_WORD_DATA` in `sdkconfig.defaults.esp-box3-xiaozhi` for production Jarvis builds.
- In `jarvis-server`, locate xiaozhi handler that receives wake + listen start; insert **Layer A** after wake audio decode, **Layer B** on full turn buffer before ASR.
