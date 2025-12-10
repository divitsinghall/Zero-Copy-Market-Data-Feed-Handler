import struct
import time
import sys
import os

def generate_large_pcap(input_file, output_file, target_size_mb=1000):
    print(f"Generating {target_size_mb}MB stress test file from {input_file}...")
    
    # 1. Read the template packets
    packets = []
    with open(input_file, 'rb') as f_in:
        # PCAP Global Header (24 bytes)
        global_header = f_in.read(24)
        
        while True:
            # Packet Header (16 bytes)
            header_bytes = f_in.read(16)
            if len(header_bytes) < 16: break
            
            # Unpack header to get length
            ts_sec, ts_usec, incl_len, orig_len = struct.unpack('=IIII', header_bytes)
            
            # Payload
            payload = f_in.read(incl_len)
            if len(payload) < incl_len: break
            
            packets.append({
                'incl_len': incl_len,
                'orig_len': orig_len,
                'payload': payload
            })

    if not packets:
        print("Error: No packets found in input file.")
        return

    print(f"Loaded {len(packets)} template packets. Multiplying...")

    # 2. Write the massive file
    with open(output_file, 'wb') as f_out:
        f_out.write(global_header)
        
        current_bytes = 24
        target_bytes = target_size_mb * 1024 * 1024
        msg_count = 0
        start_time = int(time.time())
        
        while current_bytes < target_bytes:
            for pkt in packets:
                # Synthetic Timestamp: Advance 1 second every 1 million messages
                # This tests your engine's ability to handle high throughput, not just bursts
                offset_sec = msg_count // 1_000_000
                ts_sec = start_time + offset_sec
                ts_usec = (msg_count % 1_000_000)  # Microsecond counter
                
                # Pack new header
                new_header = struct.pack('=IIII', 
                                       ts_sec, 
                                       ts_usec, 
                                       pkt['incl_len'], 
                                       pkt['orig_len'])
                
                f_out.write(new_header)
                f_out.write(pkt['payload'])
                
                current_bytes += 16 + pkt['incl_len']
                msg_count += 1
                
                if current_bytes >= target_bytes:
                    break
            
            # Progress bar
            sys.stdout.write(f"\rWritten: {current_bytes / 1024 / 1024:.2f} MB ({msg_count} msgs)")
            sys.stdout.flush()

    print(f"\n\nSUCCESS: Generated {output_file}")
    print(f"Total Orders: {msg_count}")

if __name__ == "__main__":
    # Ensure directory exists
    os.makedirs("data", exist_ok=True)
    generate_large_pcap("data/Multiple.Packets.pcap", "data/StressTest.pcap", 500) # 500MB
