#!/usr/bin/env python3
import urllib.request
import urllib.parse
import os
import sys

def upload_file(host, local_path, remote_path, chunk_size=4096):
    with open(local_path, "r", encoding="utf-8") as f:
        content = f.read()

    total_len = len(content)
    print(f"Uploading {local_path} ({total_len} bytes) to http://{host}{remote_path}...")

    for i in range(0, total_len, chunk_size):
        chunk = content[i:i+chunk_size]
        is_append = 1 if i > 0 else 0
        url = f"http://{host}/api/v1/sd/write?path={urllib.parse.quote(remote_path)}&append={is_append}"
        data = urllib.parse.urlencode({"content": chunk}).encode("utf-8")
        req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/x-www-form-urlencoded"})
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                res_body = resp.read().decode("utf-8")
                print(f"  Chunk {i//chunk_size + 1} ({len(chunk)} B): {res_body}")
        except Exception as e:
            print(f"  Error at chunk {i}: {e}")
            sys.exit(1)

    print("Upload complete!")

if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.148"
    local_file = os.path.join(os.path.dirname(__file__), "www", "index.html")
    upload_file(host, local_file, "/www/index.html")

