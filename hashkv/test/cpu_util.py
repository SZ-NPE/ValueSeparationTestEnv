import sys
import time
import psutil


def main():
    # get pid from args
    # if len(sys.argv) < 2:
    #     print("missing pid arg")
    #     sys.exit()

    # get process
    # pid = int(sys.argv[1])
    # p = psutil.Process(pid)

    # monitor process and write data to file
    interval = 1  # polling seconds
    with open("process_monitor.csv", "a+") as f:
        f.write("time,cpu%,mem%\n")  # titles
        while True:
            lines = []
            for _ in range(30):
                line = get_usage_line()
                lines.append(line)
                time.sleep(interval)
            print(lines[-1])
            f.write(''.join(lines))


# def get_usage_line(process):
#     current_time = time.strftime('%Y%m%d-%H%M%S', time.localtime(time.time()))
#     cpu_percent = process.cpu_percent()
#     mem_percent = process.memory_percent()
#     line = current_time + ',' + str(cpu_percent) + ',' + str(mem_percent) + "\n"
#     return line

def get_usage_line():
    current_time = time.strftime('%Y%m%d-%H%M%S', time.localtime(time.time()))
    cpu_percent = psutil.cpu_percent()
    mem_percent = psutil.virtual_memory().percent
    line = current_time + ',' + str(cpu_percent) + ',' + str(mem_percent) + "\n"
    return line


if __name__ == '__main__':
    main()
