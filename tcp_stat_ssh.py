import subprocess
import json
import csv
import time

# Define MTU and message size values
mtu_values = [256, 512, 1024, 2048, 4096]
message_sizes = [65536, 131072, 262144, 524288]

# Server and port configuration
server_ip = "127.0.0.1"  # Replace with the actual server's IP address if not loopback
server_ssh_user = "kali"  # Replace with your SSH username
server_ssh_host = "127.0.0.1"  # Replace with the actual server's IP or hostname
server_port = 5201      # Default iperf3 port
server_interface = "lo"  # Adjust based on the actual interface on the server

# Output CSV file
output_csv = "iperf3_results.csv"

def set_mtu_local(interface, mtu):
    """Sets the MTU value for the local network interface."""
    try:
        subprocess.run(
            ["sudo", "ifconfig", interface, "mtu", str(mtu)],
            check=True,
        )
        print(f"Set MTU to {mtu} for local interface {interface}")
    except subprocess.CalledProcessError as e:
        print(f"Failed to set MTU locally: {e}")
        
def set_mtu_remote(interface, mtu):
    """Sets the MTU value for the server's network interface via SSH."""
    try:
        # Run the command to set MTU on the remote machine without needing sudo password
        subprocess.run(
            [
                "ssh", "-o", "StrictHostKeyChecking=no",
                f"{server_ssh_user}@{server_ssh_host}",
                f"sudo ifconfig {interface} mtu {mtu}",
            ],
            check=True,
        )
        print(f"Set MTU to {mtu} for remote interface {interface}")
    except subprocess.CalledProcessError as e:
        print(f"Failed to set MTU on remote server: {e}")

def start_iperf3_server():
    """Starts the iperf3 server on the remote machine."""
    try:
        ssh_command = f"iperf3 -s -p {server_port} &"
        subprocess.run(
            [
                "ssh", "-o", "StrictHostKeyChecking=no",
                f"{server_ssh_user}@{server_ssh_host}",
                ssh_command,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        print("iperf3 server started on the remote machine.")
        time.sleep(2)  # Give the server time to start
    except Exception as e:
        print(f"Failed to start iperf3 server: {e}")

def stop_iperf3_server():
    """Stops the iperf3 server on the remote machine."""
    try:
        ssh_command = "sudo pkill -f 'iperf3 -s -p 5201'"
        subprocess.run(
            [
                "ssh", "-o", "StrictHostKeyChecking=no",
                f"{server_ssh_user}@{server_ssh_host}",
                ssh_command,
            ],
            check=True, capture_output=True, text=True
        )
        print("iperf3 server stopped on the remote machine.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to stop iperf3 server: {e}")

def run_iperf3(mtu, msg_size):
    """Runs iperf3 for the given MTU and message size."""
    cmd = [
        "iperf3",
        "-c", server_ip,  # Server address
        "-p", str(server_port),  # Port
        "-l", str(msg_size),  # Set message size
        "-t", str(3),        # Test duration
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
            # Set MTU on local client
            set_mtu_local("lo", mtu)
            
            # Set MTU on remote server
            set_mtu_remote(server_interface, mtu)
            
            # Start the iperf3 server
            start_iperf3_server()
            
            for msg_size in message_sizes:
                result = run_iperf3(mtu, msg_size)
                if result:
                    bitrate_gbps = extract_bitrate(result)
                    if bitrate_gbps is not None:
                        writer.writerow([mtu, msg_size, bitrate_gbps])
                        print(f"MTU={mtu}, Message Size={msg_size}, Bitrate={bitrate_gbps:.3f} Gbps")
            
            # Stop the iperf3 server
            stop_iperf3_server()

if __name__ == "__main__":
    main()
