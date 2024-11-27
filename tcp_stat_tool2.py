import subprocess
import json
import csv

# Define MTU and message size values
mtu_values = [256, 512, 1024, 2048, 4096]
message_sizes = [65536, 131072, 262144, 524288]

# Server and port configuration
server_ip = "10.237.175.165"  # Replace with actual server IP
server_port = 5201       # Default iperf3 port

# Output CSV file
output_csv = "iperf3_results.csv"

def set_mtu(mtu):
    """Sets the MTU value for the network interface (example: eth0)."""
    try:
        interface = "eth0"  # Change to your actual network interface
        subprocess.run(
            ["sudo", "ifconfig", interface, "mtu", str(mtu)],
            check=True,
        )
        print(f"Set MTU to {mtu} for interface {interface}")
    except subprocess.CalledProcessError as e:
        print(f"Failed to set MTU: {e}")

def run_iperf3(mtu, msg_size):
    """Runs iperf3 for the given MTU and message size."""
    cmd = [
        "iperf3",
        "-c", server_ip,  # Server address
        "-p", str(server_port),  # Port
        "-l", str(msg_size),  # Set message size
        "-t", str(3),
        "--json",            # Output results in JSON format
    ]
    try:
        print(f"Running iperf3 with MTU={mtu}, Message Size={msg_size}")
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        return json.loads(result.stdout)
    except subprocess.CalledProcessError as e:
        print(f"iperf3 failed: {e}")
        return None

def extract_bitrate(result):
    """Extracts the receiver's bits_per_second from iperf3 JSON output and converts it to Gbps."""
    try:
        bits_per_second = result["end"]["streams"][0]["receiver"]["bits_per_second"]
        return bits_per_second / 1e9  # Convert to Gbps
    except (KeyError, IndexError) as e:
        print(f"Failed to extract bitrate: {e}")
        return None

def main():
    with open(output_csv, "w", newline="") as csvfile:
        # Write CSV header
        writer = csv.writer(csvfile)
        writer.writerow(["MTU", "Message Size", "Bitrate (Gbps)"])
        
        for mtu in mtu_values:
            set_mtu(mtu)
            for msg_size in message_sizes:
                result = run_iperf3(mtu, msg_size)
                if result:
                    bitrate_gbps = extract_bitrate(result)
                    if bitrate_gbps is not None:
                        writer.writerow([mtu, msg_size, bitrate_gbps])
                        print(f"MTU={mtu}, Message Size={msg_size}, Bitrate={bitrate_gbps:.3f} Gbps")

if __name__ == "__main__":
    main()
