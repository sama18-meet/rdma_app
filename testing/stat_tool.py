import os
import csv
import socket
import psutil
import getpass
import argparse
import subprocess
from settings import dev_mtu
from settings import csv_fields
from pexpect import pxssh

# general setup
cwd = os.getcwd()
user = getpass.getuser()

# argument parsing
parser = argparse.ArgumentParser(
                    prog='RDMA PROJECT stat-tool')
parser.add_argument('testcases', type=str, help='path of csv input file')
parser.add_argument('requestor', type=str, help='machine to run requestor on')
parser.add_argument('--duration', '-D', type=int, default=2, help='duration of ib_write_bw run for each test case')
parser.add_argument('--output_file', type=str, default=cwd+"/stattool.out", help='output csv file path')

args = parser.parse_args()

testcases_file = args.testcases
output_file = args.output_file
responder = socket.gethostname()
requestor = args.requestor
duration = args.duration

# check args valid
if not os.path.isfile(testcases_file):
    print("Error: testcases file doesn't exist")
    exit()
try:
    socket.gethostbyname(requestor)
except socket.error:
    print("Error: requestor hostname could not be resolved")
    exit()
if (requestor==responder):
    print("Error: requestor and responder must be different hosts")
    exit()

# print args used
print("testcases file: {input_file}".format(input_file=testcases_file))
print("requestor: {requestor}".format(requestor=requestor))
print("duration: {duration}".format(duration=duration))
print("output file: {output_file}".format(output_file=output_file))

machine_setup_file = cwd + "/machine_setup"

# Requestor setup
Requestor = pxssh.pxssh()
if not Requestor.login (requestor, user):
    print("SSH session failed on login.")
    print(str(s))
    exit()

Requestor.sendline("source {machine_setup_file} \n".format(machine_setup_file=machine_setup_file))
Requestor.prompt()
req_dev = Requestor.before.decode("utf-8").split()[-2]     # print everything before the prompt.
print("Requestor device: {req_dev}".format(req_dev=req_dev))

# Responder setup
resp_dev = subprocess.check_output([machine_setup_file]).decode("utf-8").strip('\n')
print("Responder device: {resp_dev}".format(resp_dev=resp_dev))

# save original mtu
req_net = "eth2" if req_dev=="mlx5_0" else "eth3"
resp_net = "eth2" if resp_dev=="mlx5_0" else "eth3"
req_orig_mtu = psutil.net_if_stats()[req_net].mtu
resp_orig_mtu = psutil.net_if_stats()[resp_net].mtu


subprocess.run(['sudo', 'ip', 'link', 'set', 'dev', resp_net, 'mtu', dev_mtu])
Requestor.sendline("sudo ip link set dev {req_net} mtu {mtu}".\
        format(req_net=req_net, mtu=dev_mtu))
Requestor.prompt()
Requestor.logout()



def RunTestOnRequestor(message_size, num_qps, timeout, mtu, protocol):
    Requestor = pxssh.pxssh()
    if not Requestor.login (requestor, user):
        print("SSH session failed on login.")
        exit(-2)
    bw_avg = None
    if protocol=='RDMA':
        Requestor.sendline("ib_write_bw -d {req_dev} -D {duration} -s {message_size} -q {num_qps} -u {timeout} --mtu {mtu} --tx-depth=512 --report_gbits {responder} \n".\
                format(req_dev=req_dev, duration=duration, message_size=message_size, num_qps=num_qps, timeout=timeout, mtu=mtu, responder=responder))
        Requestor.prompt()
        bw_avg = Requestor.before.decode("utf-8").split()[-4]
    if protocol=='TCP':
        Requestor.sendline("sudo ip link set dev eth1 mtu {mtu}".format(mtu=mtu))
        Requestor.prompt()
        Requestor.sendline("iperf3 -c {responder} --bytes {message_size}".format(message_size=message_size, responder=responder))
        Requestor.prompt()
        bw_avg = Requestor.before.decode("utf-8").split()[-5]
        Requestor.sendline("sudo ip link set dev eth1 mtu {mtu}".format(mtu=dev_mtu))
    Requestor.logout()

    return bw_avg

# main loop
with open(testcases_file) as in_f:
    header = [h.strip() for h in next(in_f).split(',')]
    reader = csv.DictReader(in_f, fieldnames=header, delimiter=",", quotechar='"')
    if set(reader.fieldnames) != set(csv_fields):
        print("Error: csv file fields")
        print("Expected headers: {csv_fields}".format(csv_fields=csv_fields))
        exit()
    csv_fields.append("bw_avg")
    csv_fields.append("tcp_bw_avg")
    with open(output_file, 'w', newline='') as out_f:
        writer = csv.DictWriter(out_f, fieldnames=csv_fields, delimiter=",", quotechar='"')
        writer.writeheader()
        for row in reader:
            message_size = row['msg_size']
            num_qps = row['num_qps']
            timeout = row['timeout']
            mtu = row['mtu']

            # get RDMA bw
            subprocess.Popen(["sudo", "ib_write_bw", "-d", resp_dev, "-q", num_qps,"-s", message_size, "-u", timeout, "--mtu", mtu, "--tx-depth=512", "--report_gbits"])
            row["bw_avg"] = RunTestOnRequestor(message_size, num_qps, timeout, mtu, "RDMA")

            # get tcp bw
            subprocess.run(['sudo', 'ip', 'link', 'set', 'dev', 'eth1', 'mtu', mtu])
            subprocess.Popen(['iperf3', '-s', '-1'])
            row['tcp_bw_avg'] = RunTestOnRequestor(message_size, num_qps, timeout, mtu, "TCP")
            subprocess.run(['sudo', 'ip', 'link', 'set', 'dev', resp_net, 'mtu', dev_mtu])

            writer.writerow(row)

# reset mtu
Requestor = pxssh.pxssh()
if not Requestor.login (requestor, user):
    print("SSH session failed on login.")
    exit(-2)
Requestor.sendline("sudo ip link set dev {req_net} mtu {mtu}".format(req_net=req_net, mtu=req_orig_mtu))
Requestor.prompt()
Requestor.logout()

subprocess.run(['sudo', 'ip', 'link', 'set', 'dev', resp_net, 'mtu', str(resp_orig_mtu)])

