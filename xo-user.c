#include <fcntl.h>
#include <getopt.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"
#define MY_IOCTL_MAGIC_MCTS 'O'
#define MY_IOCTL_MCTS _IOWR(MY_IOCTL_MAGIC_MCTS, 1, unsigned int)
#define MY_IOCTL_MAGIC_NEGAMAX 'X'
#define MY_IOCTL_NEGAMAX _IOWR(MY_IOCTL_MAGIC_NEGAMAX, 1, unsigned int)


static unsigned int user_draw_buffer;
static int finish;

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~IXON;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool read_attr, end_attr;

static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("Stopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("Stopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}


static int draw_board(unsigned int display_buf, unsigned int user_display_buf)
{
    const char mapping[3] = {'O', 'X', ' '};
    for (int i = 0; i < BOARD_SIZE; i++) {
        putchar(mapping[display_buf & 3]);
        display_buf >>= 2;
        for (int j = 0; j < BOARD_SIZE - 1; j++) {
            putchar('|');
            putchar(mapping[display_buf & 3]);
            display_buf >>= 2;
        }
        putchar('\n');
        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            putchar('-');
        }
        putchar('\n');
    }

    for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
        putchar('=');
    }
    putchar('\n');
    for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
        putchar('=');
    }
    putchar('\n');


    for (int i = 0; i < BOARD_SIZE; i++) {
        putchar(mapping[user_display_buf & 3]);
        user_display_buf >>= 2;
        for (int j = 0; j < BOARD_SIZE - 1; j++) {
            putchar('|');
            putchar(mapping[user_display_buf & 3]);
            user_display_buf >>= 2;
        }
        putchar('\n');
        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            putchar('-');
        }
        putchar('\n');
    }

    if (finish == 16) {
        finish = 0;
        user_draw_buffer = 0b10101010101010101010101010101010;
    }

    return 0;
}

void task_mtcs(int fd)
{
    finish++;
    unsigned int move = user_draw_buffer;
    ioctl(fd, MY_IOCTL_MCTS, &move);
    if (move != -1) {
        user_draw_buffer &= ~(0b11 << (move << 1));
    }
}

void task_negamax(int fd)
{
    finish++;
    unsigned int move = user_draw_buffer;
    ioctl(fd, MY_IOCTL_NEGAMAX, &move);

    if (move != -1) {
        user_draw_buffer &= ~(0b11 << (move << 1));
        user_draw_buffer |= (0b01 << (move << 1));
    }
}

int main(int argc, char *argv[])
{
    user_draw_buffer = 0b10101010101010101010101010101010;
    finish = 0;

    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    unsigned int display_buf;

    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDWR);
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;



    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);

        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {
            printf("Error with select system call\n");
            exit(1);
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (read_attr && FD_ISSET(device_fd, &readset)) {
            FD_CLR(device_fd, &readset);
            printf("\033[H\033[J"); /* ASCII escape code to clear the screen */
            read(device_fd, &display_buf, sizeof(display_buf));

            task_mtcs(device_fd);
            task_negamax(device_fd);

            draw_board(display_buf, user_draw_buffer);
        }
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
