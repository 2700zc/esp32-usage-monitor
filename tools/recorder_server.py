#!/usr/bin/env python3
import asyncio
import json
import os
import sys
import argparse
from datetime import datetime
import struct

import websockets

def make_wav_header(pcm_len, sample_rate=16000, bits_per_sample=16, channels=1):
    byte_rate = sample_rate * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8
    data_size = pcm_len
    file_size = 36 + data_size
    header = bytearray()
    header += b'RIFF'
    header += struct.pack('<I', file_size)
    header += b'WAVE'
    header += b'fmt '
    header += struct.pack('<I', 16)
    header += struct.pack('<H', 1)
    header += struct.pack('<H', channels)
    header += struct.pack('<I', sample_rate)
    header += struct.pack('<I', byte_rate)
    header += struct.pack('<H', block_align)
    header += struct.pack('<H', bits_per_sample)
    header += b'data'
    header += struct.pack('<I', data_size)
    return header

async def handler(websocket):
    pcm_buf = bytearray()
    save_path = None
    try:
        async for msg in websocket:
            if isinstance(msg, str):
                try:
                    data = json.loads(msg)
                except json.JSONDecodeError:
                    await websocket.send(json.dumps({"type":"error","msg":"invalid json"}))
                    continue
                if data["type"] == "start":
                    pcm_buf = bytearray()
                    print("recording started")
                elif data["type"] == "stop":
                    now = datetime.now()
                    filename = f"rec_{now.strftime('%Y%m%d_%H%M%S')}.wav"
                    save_path = os.path.join(args.dir, filename)
                    os.makedirs(args.dir, exist_ok=True)
                    wav = bytearray(make_wav_header(len(pcm_buf)))
                    wav.extend(pcm_buf)
                    with open(save_path, "wb") as f:
                        f.write(wav)
                    print(f"saved {save_path} ({len(pcm_buf)} bytes PCM)")
                    await websocket.send(json.dumps({"type": "saved", "path": filename}))
                else:
                    await websocket.send(json.dumps({"type":"error","msg":"unknown message type"}))
            elif isinstance(msg, bytes):
                pcm_buf.extend(msg)
    except websockets.exceptions.ConnectionClosed:
        pass
    except Exception as e:
        try:
            await websocket.send(json.dumps({"type":"error","msg":str(e)}))
        except Exception:
            pass

async def main():
    global args
    parser = argparse.ArgumentParser(description="ESP32 Recorder Server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=12345)
    parser.add_argument("--dir", default="./recordings")
    args = parser.parse_args()

    print(f"recorder server listening on {args.host}:{args.port}")
    async with websockets.serve(handler, args.host, args.port):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
