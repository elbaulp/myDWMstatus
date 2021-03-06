#define _BSD_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <X11/Xlib.h>

char *tzpst = "Europe/Madrid";
static const char *suffixes[] = {"KiB", "MiB", "GiB", "TiB", "PiB", ""};

static Display *dpy;

char *
smprintf(char *fmt, ...) {
    va_list fmtargs;
    char *ret;
    int len;

    va_start(fmtargs, fmt);
    len = vsnprintf(NULL, 0, fmt, fmtargs);
    va_end(fmtargs);

    ret = malloc(++len);
    if (ret == NULL) {
        perror("malloc");
        exit(1);
    }

    va_start(fmtargs, fmt);
    vsnprintf(ret, len, fmt, fmtargs);
    va_end(fmtargs);

    return ret;
}

void settz(char *tzname) {
    setenv("TZ", tzname, 1);
}

char *
mktimes(char *fmt, char *tzname) {
    char buf[129];
    time_t tim;
    struct tm *timtm;

    bzero(buf, sizeof (buf));
    settz(tzname);
    tim = time(NULL);
    timtm = localtime(&tim);
    if (timtm == NULL) {
        perror("localtime");
        exit(1);
    }

    if (!strftime(buf, sizeof (buf) - 1, fmt, timtm)) {
        fprintf(stderr, "strftime == 0\n");
        exit(1);
    }

    return smprintf("%s", buf);
}

void setstatus(char *str) {
    XStoreName(dpy, DefaultRootWindow(dpy), str);
    XSync(dpy, False);
}

char *
loadavg(void) {
    double avgs[3];

    if (getloadavg(avgs, 3) < 0) {
        perror("getloadavg");
        exit(1);
    }

    return smprintf("%.2f %.2f %.2f", avgs[0], avgs[1], avgs[2]);
}

char *
getbattery(char *path) {
    char batteryLevel[5];
    char *batStatus = 0;
    FILE *fd;

    fd = fopen(path, "r");
    if (fd != NULL) {

        fgets(batteryLevel, 3, fd);
        int batPercent = atoi(batteryLevel);
        fclose(fd);


        if (batPercent > 80) {
            batStatus = smprintf("%s %d%%%c", "", batPercent, '\x05');
        } else if (batPercent > 50) {
            batStatus = smprintf("%s %d%%%c", "", batPercent, '\x06');
        } else {
            batStatus = smprintf("%s %d%%%c", "", batPercent, '\x07');
        }

    } else {
        batStatus = smprintf("%s%c", "", '\x05');
    }

    return batStatus;
}

char*
runcmd(char* cmd) {
    FILE* fp = popen(cmd, "r");
    if (fp == NULL)
        return NULL;
    char ln[30];
    fgets(ln, sizeof (ln) - 1, fp);
    pclose(fp);
    ln[strlen(ln) - 1] = '\0';
    return smprintf("%s", ln);
}

static unsigned long long lastTotalUser[4], lastTotalUserLow[4],
lastTotalSys[4], lastTotalIdle[4];

char trash[5];

void initcore() {
    FILE* file = fopen("/proc/stat", "r");
    char ln[100];

    for (int i = 0; i < 5; i++) {
        fgets(ln, 99, file);
        if (i < 1)
            continue;
        sscanf(ln, "%s %llu %llu %llu %llu", trash, &lastTotalUser[i - 1],
                &lastTotalUserLow[i - 1], &lastTotalSys[i - 1],
                &lastTotalIdle[i - 1]);
    }
    fclose(file);

}

void getcore(char cores[4][6]) {
    double percent;
    FILE* file;
    unsigned long long totalUser[4], totalUserLow[4], totalSys[4], totalIdle[4],
            total[2];

    char ln[100];

    file = fopen("/proc/stat", "r");
    for (int i = 0; i < 5; i++) {
        fgets(ln, 99, file);
        if (i < 1)
            continue;
        sscanf(ln, "%s %llu %llu %llu %llu", trash, &totalUser[i - 1],
                &totalUserLow[i - 1], &totalSys[i - 1], &totalIdle[i - 1]);
    }
    fclose(file);

    for (int i = 0; i < 4; i++) {
        if (totalUser[i] < lastTotalUser[i] || totalUserLow[i] < lastTotalUserLow[i]
                || totalSys[i] < lastTotalSys[i] || totalIdle[i] < lastTotalIdle[i]) {
            //Overflow detection. Just skip this value.
            percent = -1.0;
        } else {
            total[i] = (totalUser[i] - lastTotalUser[i])
                    + (totalUserLow[i] - lastTotalUserLow[i])
                    + (totalSys[i] - lastTotalSys[i]);
            percent = total[i];
            total[i] += (totalIdle[i] - lastTotalIdle[i]);
            percent /= total[i];
            percent *= 100;
        }

        if (percent > 70) {
            strcpy(cores[i], smprintf("%d%%%c", (int) percent, '\x07'));
        } else if (percent > 50) {
            strcpy(cores[i], smprintf("%d%%%c", (int) percent, '\x06'));
        } else {
            strcpy(cores[i], smprintf("%d%%%c", (int) percent, '\x05'));
        }
    }

    for (int i = 0; i < 4; i++) {
        lastTotalUser[i] = totalUser[i];
        lastTotalUserLow[i] = totalUserLow[i];
        lastTotalSys[i] = totalSys[i];
        lastTotalIdle[i] = totalIdle[i];
    }

}

char *gettemp() {
    FILE *fd;
    int temp;

    fd = fopen("/sys/class/hwmon/hwmon0/temp1_input", "r");
    fscanf(fd, "%d", &temp);
    fclose(fd);
    temp /= 1000;

    if (temp > 55) {
        return smprintf("%dc\x07", temp);
    } else if (temp > 45) {
        return smprintf("%dc\x06", temp);
    } else {
        return smprintf("%dc\x05", temp);
    }
}

char *getmem() {
    FILE *fd;
    long total, free, avail, buf, cache, use;
    int used;
    const char **suffix = suffixes;

    fd = fopen("/proc/meminfo", "r");
    fscanf(fd,
            "MemTotal: %ld kB\nMemFree: %ld kB\nMemAvailable: %ld kB\nBuffers: %ld kB\nCached: %ld kB\n",
            &total, &free, &avail, &buf, &cache);
    fclose(fd);
    use = total - avail - buf;
    used = 100 * (use) / total;

    // Use suffixes like conky
    while (llabs(use / 1024) >= 1000LL && **(suffix + 2)) {
        use /= 1024;
        suffix++;
    }

    suffix++;
    float fuse = use / 1024.0;

    if (used > 70) {
        return smprintf("%d%% (%.2f %s)\x07", used, fuse, *suffix);
    } else if (used > 50) {
        return smprintf("%d%% (%.2f %s)\x06", used, fuse, *suffix);
    } else {
        return smprintf("%d%% (%.2f %s)\x05", used, fuse, *suffix);
    }
}

#define BATTERY "/sys/class/power_supply/BAT0/capacity"
#define ADAPTER "/sys/class/power_supply/AC0/online"
#define VOLCMD "echo $(amixer get Master | tail -n1 | sed -r 's/.*\\[(.*)%\\].*/\\1/')%"
#define MEMCMD "echo $(free -m | awk '/buffers\\/cache/ {print $3}')M"
#define RXWCMD "cat /sys/class/net/wlan0/statistics/rx_bytes"
#define TXWCMD "cat /sys/class/net/wlan0/statistics/tx_bytes"
#define RXCMD "cat /sys/class/net/eth0/statistics/rx_bytes"
#define TXCMD "cat /sys/class/net/eth0/statistics/tx_bytes"

int main(void) {
    char *status;
    //  char *avgs;
    char *bat;
    char *date;
    char *tme;
    char* vol;
    char cores[4][6];
    char *mem;
    char *rx_old, *rx_now, *tx_old, *tx_now, *rxw_old, *rxw_now, *txw_old, *txw_now;
    char *temp;
    initcore();
    int rx_rate, tx_rate, rxw_rate, txw_rate; //kilo bytes
    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "dwmstatus: cannot open display.\n");
        return 1;
    }
    rx_old = runcmd(RXCMD);
    tx_old = runcmd(TXCMD);
    rxw_old = runcmd(RXWCMD);
    txw_old = runcmd(TXWCMD);
    for (;; sleep(1)) {
        //avgs = loadavg();
        bat = getbattery(BATTERY);
        date = mktimes("%D", tzpst);
        tme = mktimes("%k.%M", tzpst);
        vol = runcmd(VOLCMD);
        mem = getmem(); //runcmd(MEMCMD);
        //get transmitted and recv'd bytes
        rx_now = runcmd(RXCMD);
        tx_now = runcmd(TXCMD);
        rxw_now = runcmd(RXWCMD);
        txw_now = runcmd(TXWCMD);
        rx_rate = (atoi(rx_now) - atoi(rx_old)) / 1024;
        tx_rate = (atoi(tx_now) - atoi(tx_old)) / 1024;
        rxw_rate = (atoi(rxw_now) - atoi(rxw_old)) / 1024;
        txw_rate = (atoi(txw_now) - atoi(txw_old)) / 1024;
        getcore(cores);
        temp = gettemp();
        status =
                smprintf(
                "[\x01 %s ][ \x01  %dK\x02 /\x01 %dK\x02 |  %dK\x02 /\x01 %dK \x02][\x01  %s\x04 ][\x01  %s /\x01 %s /\x01 %s /\x01 %s ][\x01  %s ][\x01  %s\x03 ][\x01  %s | %s ]\x01",
                bat, rxw_rate, txw_rate, rx_rate, tx_rate, vol, cores[0], cores[1], cores[2], cores[3], temp, mem, date, tme);
        strcpy(rx_old, rx_now);
        strcpy(tx_old, tx_now);
        strcpy(rxw_old, rxw_now);
        strcpy(txw_old, txw_now);
        //printf("%s\n", status);
        setstatus(status);
        //free(avgs);
        free(rx_now);
        free(tx_now);
        free(rxw_now);
        free(txw_now);
        free(bat);
        free(vol);
        free(date);
        free(status);
        free(temp);
        free(mem);
    }

    XCloseDisplay(dpy);

    return 0;
}
