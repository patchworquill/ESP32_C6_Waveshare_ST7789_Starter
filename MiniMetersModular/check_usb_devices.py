import subprocess
text = subprocess.check_output(["ioreg", "-w", "0", "-l"], text=True, errors="replace")
lines = text.splitlines()
# find tty line and print a window of class lines only
for i, line in enumerate(lines):
    if "usbmodemmmm_db1c1" in line or ("mmm-db1c" in line and "IOUSBHostInterface" in line):
        start = max(0, i-40)
        end = min(len(lines), i+15)
        print(f"\n===== around {i} =====")
        for j in range(start, end):
            l = lines[j]
            if l.strip().startswith("+-") or l.strip().startswith("|") and "class " in l or "bInterface" in l or "USB Interface" in l or "Product Name" in l or "IOTTY" in l or "bInterfaceClass" in l or "bNumEndpoints" in l or "IOClass" in l:
                print(f"{j}: {l[:220]}")