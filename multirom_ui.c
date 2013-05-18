#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

#include "multirom_ui.h"
#include "framebuffer.h"
#include "input.h"
#include "log.h"
#include "listview.h"
#include "util.h"
#include "button.h"
#include "checkbox.h"
#include "version.h"
#include "pong.h"
#include "progressdots.h"

#define HEADER_HEIGHT 65
#define TAB_BTN_WIDTH 100

static fb_text *tab_texts[TAB_COUNT] = { 0 };
static fb_rect *selected_tab_rect = NULL;
static button *tab_btns[TAB_COUNT] = { NULL };
static int selected_tab = -1;
static void *tab_data = NULL;
static struct multirom_status *mrom_status = NULL;
static struct multirom_rom *selected_rom = NULL;
static volatile int exit_ui_code = -1;
static fb_msgbox *active_msgbox = NULL;
static volatile int loop_act = 0;
static button *pong_btn = NULL;

static pthread_mutex_t exit_code_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t CLR_PRIMARY = LBLUE;
uint32_t CLR_SECONDARY = LBLUE2;

#define LOOP_UPDATE_USB 0x01
#define LOOP_START_PONG 0x02
#define LOOP_CHANGE_CLR 0x04

static void list_block(char *path, int rec)
{
    ERROR("Listing %s", path);
    DIR *d = opendir(path);
    if(!d)
    {
        ERROR("Failed to open %s", path);
        return;
    }

    struct dirent *dr;
    struct stat info;
    while((dr = readdir(d)))
    {
        if(dr->d_name[0] == '.')
            continue;

        ERROR("%s/%s (%d)", path, dr->d_name, dr->d_type);
        if(dr->d_type == 4 && rec)
        {
            char name[256];
            sprintf(name, "%s/%s", path, dr->d_name);
            list_block(name, 1);
        }
    }

    closedir(d);
}

int multirom_ui(struct multirom_status *s, struct multirom_rom **to_boot)
{
    if(multirom_init_fb() < 0)
        return UI_EXIT_BOOT_ROM;

    fb_freeze(1);

    mrom_status = s;

    exit_ui_code = -1;
    selected_rom = NULL;
    active_msgbox = NULL;

    multirom_ui_setup_colors(s->colors, &CLR_PRIMARY, &CLR_SECONDARY);

    selected_tab = -1;
    multirom_ui_init_header();
    multirom_ui_switch(TAB_INTERNAL);

    add_touch_handler(&multirom_ui_touch_handler, NULL);
    start_input_thread();

    multirom_set_brightness(s->brightness);

    fb_freeze(0);

    if(s->auto_boot_rom && s->auto_boot_seconds > 0)
        multirom_ui_auto_boot();
    else
        fb_draw();

    while(1)
    {
        pthread_mutex_lock(&exit_code_mutex);
        if(exit_ui_code != -1)
        {
            pthread_mutex_unlock(&exit_code_mutex);
            break;
        }

        if(loop_act & LOOP_UPDATE_USB)
        {
            multirom_find_usb_roms(mrom_status);
            if(selected_tab == TAB_USB)
                multirom_ui_tab_rom_update_usb(tab_data);
            loop_act &= ~(LOOP_UPDATE_USB);
        }

        if(loop_act & LOOP_START_PONG)
        {
            loop_act &= ~(LOOP_START_PONG);
            input_push_context();
            fb_push_context();

            pong();

            fb_pop_context();
            input_pop_context();
        }

        if(loop_act & LOOP_CHANGE_CLR)
        {
            fb_freeze(1);

            multirom_ui_setup_colors(s->colors, &CLR_PRIMARY, &CLR_SECONDARY);

            // force redraw tab
            int tab = selected_tab;
            selected_tab = -1;

            multirom_ui_destroy_tab(tab);
            multirom_ui_switch(tab);

            fb_freeze(0);
            fb_draw();

            loop_act &= ~(LOOP_CHANGE_CLR);
        }

        pthread_mutex_unlock(&exit_code_mutex);

        usleep(100000);
    }

    rm_touch_handler(&multirom_ui_touch_handler, NULL);

    fb_create_msgbox(500, 250, CLR_PRIMARY);

    switch(exit_ui_code)
    {
        case UI_EXIT_BOOT_ROM:
            *to_boot = selected_rom;
            fb_msgbox_add_text(-1, 40, SIZE_BIG, "Booting ROM...");
            fb_msgbox_add_text(-1, -1, SIZE_NORMAL, selected_rom->name);
            break;
        case UI_EXIT_REBOOT:
        case UI_EXIT_REBOOT_RECOVERY:
        case UI_EXIT_REBOOT_BOOTLOADER:
            fb_msgbox_add_text(-1, -1, SIZE_BIG, "Rebooting...");
            break;
        case UI_EXIT_SHUTDOWN:
            fb_msgbox_add_text(-1, -1, SIZE_BIG, "Shutting down...");
            break;
    }

    fb_draw();
    fb_freeze(1);

    button_destroy(pong_btn);
    pong_btn = NULL;

    int i;
    for(i = 0; i < TAB_COUNT; ++i)
    {
        button_destroy(tab_btns[i]);
        tab_btns[i] = NULL;
    }

    stop_input_thread();

    multirom_ui_destroy_tab(selected_tab);
    fb_clear();
    fb_close();

    return exit_ui_code;
}


void multirom_ui_setup_colors(int clr, uint32_t *primary, uint32_t *secondary)
{
    static const int clrs[][2] = {
        // Primary,   Secondary - OxAAGGBBRR
        { LBLUE,      LBLUE2 },     // CLRS_BLUE
        { 0xFFCC66AA, 0xFFCC89B6 }, // CLRS_PURPLE
        { 0xFF00BD8A, 0xFF51F2C9 }, // CLRS_GREEN
        { 0xFF008AFF, 0xFF51AEFF }, // CLRS_ORANGE
        { 0xFF0000CC, 0xFF6363FF }, // CLRS_RED
        { 0xFF2F5EB8, 0xFF689CFF }, // CLRS_BROWN
    };

    if(clr < 0 || clr >= (int)ARRAY_SIZE(clrs))
        clr = 0;

    *primary = clrs[clr][0];
    *secondary = clrs[clr][1];
}

void multirom_ui_init_header(void)
{
    int i, text_x, text_y;
    int x = fb_width - (TAB_BTN_WIDTH*TAB_COUNT);

    static const char *str[] = { "Internal", "USB", "Misc", "MultiROM" };

    text_x = center_x(0, x, SIZE_EXTRA, str[3]);
    fb_add_text(text_x, 5, WHITE, SIZE_EXTRA, str[3]);

    pong_btn = malloc(sizeof(button));
    memset(pong_btn, 0, sizeof(button));
    pong_btn->w = x;
    pong_btn->h = HEADER_HEIGHT;
    pong_btn->clicked = &multirom_ui_start_pong;
    button_init_ui(pong_btn, NULL, 0);

    for(i = 0; i < TAB_COUNT; ++i)
    {
        text_x = center_x(x, TAB_BTN_WIDTH, SIZE_NORMAL, str[i]);
        text_y = center_y(0, HEADER_HEIGHT, SIZE_NORMAL);
        tab_texts[i] = fb_add_text(text_x, text_y, WHITE, SIZE_NORMAL, str[i]);

        fb_add_rect(x, 0, 2, HEADER_HEIGHT, WHITE);

        tab_btns[i] = malloc(sizeof(button));
        memset(tab_btns[i], 0, sizeof(button));
        tab_btns[i]->x = x;
        tab_btns[i]->w = TAB_BTN_WIDTH;
        tab_btns[i]->h = HEADER_HEIGHT;
        tab_btns[i]->action = i;
        tab_btns[i]->clicked = &multirom_ui_switch;
        button_init_ui(tab_btns[i], NULL, 0);

        x += TAB_BTN_WIDTH;
    }

    fb_add_rect(0, HEADER_HEIGHT, fb_width, 2, WHITE);
}

void multirom_ui_header_select(int tab)
{
    int i;
    for(i = 0; i < TAB_COUNT; ++i)
        tab_texts[i]->color = (i == tab) ? BLACK : WHITE;

    if(!selected_tab_rect)
        selected_tab_rect = fb_add_rect(0, 0, TAB_BTN_WIDTH, HEADER_HEIGHT, WHITE);

    selected_tab_rect->head.x = fb_width - (TAB_BTN_WIDTH * (TAB_COUNT - tab));
}

void multirom_ui_destroy_tab(int tab)
{
    switch(tab)
    {
        case -1:
            break;
        case TAB_USB:
        case TAB_INTERNAL:
            multirom_ui_tab_rom_destroy(tab_data);
            break;
        case TAB_MISC:
            multirom_ui_tab_misc_destroy(tab_data);
            break;
        default:
            assert(0);
            break;
    }
    tab_data = NULL;
}

void multirom_ui_switch(int tab)
{
    if(tab == selected_tab)
        return;

    fb_freeze(1);

    multirom_ui_header_select(tab);

    // destroy old tab
    multirom_ui_destroy_tab(selected_tab);

    // init new tab
    switch(tab)
    {
        case TAB_USB:
        case TAB_INTERNAL:
            tab_data = multirom_ui_tab_rom_init(tab);
            break;
        case TAB_MISC:
            tab_data = multirom_ui_tab_misc_init();
            break;
    }

    selected_tab = tab;

    fb_freeze(0);
    fb_draw();
}

void multirom_ui_fill_rom_list(listview *view, int mask)
{
    int i;
    struct multirom_rom *rom;
    void *data;
    listview_item *it;
    char part_desc[64];
    for(i = 0; mrom_status->roms && mrom_status->roms[i]; ++i)
    {
        rom = mrom_status->roms[i];

        if(!(M(rom->type) & mask))
            continue;

        if(rom->partition)
            sprintf(part_desc, "%s (%s)", rom->partition->name, rom->partition->fs);

        data = rom_item_create(rom->name, rom->partition ? part_desc : NULL);
        it = listview_add_item(view, rom->id, data);

        if ((mrom_status->auto_boot_rom && rom == mrom_status->auto_boot_rom) ||
            (!mrom_status->auto_boot_rom && rom == mrom_status->current_rom))
        {
            listview_select_item(view, it);
        }
    }

    if(view->items != NULL && view->selected == NULL)
        listview_select_item(view, view->items[0]);
}

int multirom_ui_touch_handler(touch_event *ev, void *data)
{
    static int touch_count = 0;
    if(ev->changed & TCHNG_ADDED)
    {
        if(++touch_count == 4)
        {
            multirom_take_screenshot();
            touch_count = 0;
        }

        if(active_msgbox)
        {
            pthread_mutex_lock(&exit_code_mutex);
            fb_destroy_msgbox();
            fb_freeze(0);
            fb_draw();
            active_msgbox = NULL;
            set_touch_handlers_mode(HANDLERS_FIRST);
            pthread_mutex_unlock(&exit_code_mutex);
        }
    }

    if((ev->changed & TCHNG_REMOVED) && touch_count > 0)
        --touch_count;

    return -1;
}

void multirom_ui_auto_boot(void)
{
    int seconds = mrom_status->auto_boot_seconds*1000;
    active_msgbox = fb_create_msgbox(350, 165, CLR_PRIMARY);

    fb_msgbox_add_text(-1, 20, SIZE_BIG, "Auto-boot");
    fb_msgbox_add_text(-1, active_msgbox->h-100, SIZE_NORMAL, "ROM: %s", mrom_status->auto_boot_rom->name);
    fb_msgbox_add_text(-1, active_msgbox->h-60, SIZE_NORMAL, "Touch anywhere to cancel");

    fb_text *sec_text = fb_msgbox_add_text(-1, -1, SIZE_BIG, "%d", seconds/1000);

    fb_draw();
    fb_freeze(1);
    set_touch_handlers_mode(HANDLERS_ALL);

    while(1)
    {
        pthread_mutex_lock(&exit_code_mutex);
        if(!active_msgbox)
        {
            pthread_mutex_unlock(&exit_code_mutex);
            break;
        }
        pthread_mutex_unlock(&exit_code_mutex);

        seconds -= 50;
        if(seconds <= 0)
        {
            pthread_mutex_lock(&exit_code_mutex);
            selected_rom = mrom_status->auto_boot_rom;
            active_msgbox = NULL;
            exit_ui_code = UI_EXIT_BOOT_ROM;
            pthread_mutex_unlock(&exit_code_mutex);
            fb_destroy_msgbox();
            fb_freeze(0);
            break;
        }
        else if((seconds+50)/1000 != seconds/1000)
        {
            sprintf(sec_text->text, "%d", seconds/1000);
            fb_freeze(0);
            fb_draw();
            fb_freeze(1);
        }
        usleep(50000);
    }
    set_touch_handlers_mode(HANDLERS_FIRST);
}

void multirom_ui_refresh_usb_handler(void)
{
    pthread_mutex_lock(&exit_code_mutex);
    loop_act |= LOOP_UPDATE_USB;
    pthread_mutex_unlock(&exit_code_mutex);
}

void multirom_ui_start_pong(int action)
{
    pthread_mutex_lock(&exit_code_mutex);
    loop_act |= LOOP_START_PONG;
    pthread_mutex_unlock(&exit_code_mutex);
}

#define ROMS_FOOTER_H 130
#define ROMS_HEADER_H 90

#define BOOTBTN_W 300
#define BOOTBTN_H 80

#define REFRESHBTN_W 400
#define REFRESHBTN_H 60

typedef struct 
{
    listview *list;
    button **buttons;
    void **ui_elements;
    fb_text *rom_name;
    fb_text *title_text;
    fb_text *usb_text;
    button *boot_btn;
    progdots *usb_prog;
} tab_roms;

void *multirom_ui_tab_rom_init(int tab_type)
{
    tab_roms *t = malloc(sizeof(tab_roms));
    memset(t, 0, sizeof(tab_roms));

    int base_y = fb_height-ROMS_FOOTER_H;

    // must be before list
    tab_data = (void*)t;
    t->rom_name = fb_add_text(0, center_y(base_y, ROMS_FOOTER_H, SIZE_NORMAL),
                              WHITE, SIZE_NORMAL, "");


    // rom list
    t->list = malloc(sizeof(listview));
    memset(t->list, 0, sizeof(listview));
    t->list->y = HEADER_HEIGHT+ROMS_HEADER_H;
    t->list->w = fb_width;
    t->list->h = fb_height - t->list->y - ROMS_FOOTER_H-20;

    t->list->item_draw = &rom_item_draw;
    t->list->item_hide = &rom_item_hide;
    t->list->item_height = &rom_item_height;
    t->list->item_destroy = &rom_item_destroy;
    t->list->item_selected = &multirom_ui_tab_rom_selected;

    listview_init_ui(t->list);

    if(tab_type == TAB_INTERNAL)
        multirom_ui_fill_rom_list(t->list, MASK_INTERNAL);

    listview_update_ui(t->list);

    int has_roms = (int)(t->list->items == NULL);

    // header
    int y = center_y(HEADER_HEIGHT, ROMS_HEADER_H, SIZE_BIG);
    t->title_text = fb_add_text(0, y, CLR_PRIMARY, SIZE_BIG, "");
    list_add(t->title_text, &t->ui_elements);

    multirom_ui_tab_rom_set_empty((void*)t, has_roms);

    // footer
    fb_rect *sep = fb_add_rect(0, fb_height-ROMS_FOOTER_H, fb_width, 2, CLR_PRIMARY);
    list_add(sep, &t->ui_elements);

    button *b = malloc(sizeof(button));
    memset(b, 0, sizeof(button));
    b->x = fb_width - BOOTBTN_W - 20;
    b->y = base_y + (ROMS_FOOTER_H-BOOTBTN_H)/2;
    b->w = BOOTBTN_W;
    b->h = BOOTBTN_H;
    b->clicked = &multirom_ui_tab_rom_boot_btn;
    button_init_ui(b, "Boot", SIZE_BIG);
    button_enable(b, !has_roms);
    list_add(b, &t->buttons);
    t->boot_btn = b;

    if(tab_type == TAB_USB)
    {
        multirom_set_usb_refresh_handler(&multirom_ui_refresh_usb_handler);
        multirom_set_usb_refresh_thread(mrom_status, 1);
    }
    return t;
}

void multirom_ui_tab_rom_destroy(void *data)
{
    multirom_set_usb_refresh_thread(mrom_status, 0);

    tab_roms *t = (tab_roms*)data;

    list_clear(&t->buttons, &button_destroy);
    list_clear(&t->ui_elements, &fb_remove_item);

    listview_destroy(t->list);

    fb_rm_text(t->rom_name);

    if(t->usb_prog)
        progdots_destroy(t->usb_prog);

    free(t);
}

void multirom_ui_tab_rom_selected(listview_item *prev, listview_item *now)
{
    struct multirom_rom *rom = multirom_get_rom_by_id(mrom_status, now->id);
    if(!rom || !tab_data)
        return;

    tab_roms *t = (tab_roms*)tab_data;

    free(t->rom_name->text);
    t->rom_name->text = malloc(strlen(rom->name)+1);
    strcpy(t->rom_name->text, rom->name);

    t->rom_name->head.x = center_x(0, fb_width-BOOTBTN_W-20, SIZE_NORMAL, rom->name);

    fb_draw();
}

void multirom_ui_tab_rom_boot_btn(int action)
{
    if(!tab_data)
        return;

    tab_roms *t = (tab_roms*)tab_data;
    if(!t->list->selected)
        return;

    struct multirom_rom *rom = multirom_get_rom_by_id(mrom_status, t->list->selected->id);
    if(!rom)
        return;

    int m = M(rom->type);
    if(m & MASK_UNSUPPORTED)
    {
        active_msgbox = fb_create_msgbox(550, 360, DRED);
        fb_msgbox_add_text(-1, 30, SIZE_BIG, "Error");
        fb_msgbox_add_text(-1, 90, SIZE_NORMAL, "Unsupported ROM type.");
        fb_msgbox_add_text(-1, 180, SIZE_NORMAL, "See XDA thread for more info.");
        fb_msgbox_add_text(-1, active_msgbox->h-60, SIZE_NORMAL, "Touch anywhere to close");

        fb_draw();
        fb_freeze(1);
        set_touch_handlers_mode(HANDLERS_ALL);
        return;
    }

    if (((m & MASK_KEXEC) || ((m & MASK_ANDROID) && rom->has_bootimg)) &&
        multirom_has_kexec() != 0)
    {
        active_msgbox = fb_create_msgbox(550, 360, DRED);
        fb_msgbox_add_text(-1, 30, SIZE_BIG, "Error");
        fb_msgbox_add_text(-1, 90, SIZE_NORMAL, "Kexec-hardboot support");
        fb_msgbox_add_text(-1, 125, SIZE_NORMAL, "required to boot this ROM.");
        fb_msgbox_add_text(-1, 180, SIZE_NORMAL, "Use kernel with");
        fb_msgbox_add_text(-1, 215, SIZE_NORMAL, "kexec-hardboot support.");
        fb_msgbox_add_text(-1, active_msgbox->h-60, SIZE_NORMAL, "Touch anywhere to close");

        fb_draw();
        fb_freeze(1);
        set_touch_handlers_mode(HANDLERS_ALL);
        return;
    }

    if((m & MASK_KEXEC) && strchr(rom->name, ' '))
    {
        active_msgbox = fb_create_msgbox(550, 360, DRED);
        fb_msgbox_add_text(-1, 30, SIZE_BIG, "Error");
        fb_msgbox_add_text(-1, 90, SIZE_NORMAL, "ROM's name contains spaces");
        fb_msgbox_add_text(-1, 180, SIZE_NORMAL, "Remove spaces from ROM's name");
        fb_msgbox_add_text(-1, active_msgbox->h-60, SIZE_NORMAL, "Touch anywhere to close");

        fb_draw();
        fb_freeze(1);
        set_touch_handlers_mode(HANDLERS_ALL);
        return;
    }

    pthread_mutex_lock(&exit_code_mutex);
    selected_rom = rom;
    exit_ui_code = UI_EXIT_BOOT_ROM;
    pthread_mutex_unlock(&exit_code_mutex);
}

void multirom_ui_tab_rom_update_usb(void *data)
{
    tab_roms *t = (tab_roms*)tab_data;
    listview_clear(t->list);

    t->rom_name->text = realloc(t->rom_name->text, 1);
    t->rom_name->text[0] = 0;

    multirom_ui_fill_rom_list(t->list, MASK_USB_ROMS);
    listview_update_ui(t->list);

    multirom_ui_tab_rom_set_empty(data, (int)(t->list->items == NULL));
    fb_draw();
}

void multirom_ui_tab_rom_refresh_usb(int action)
{
    multirom_update_partitions(mrom_status);
}

void multirom_ui_tab_rom_set_empty(void *data, int empty)
{
    assert(empty == 0 || empty == 1);
    tab_roms *t = (tab_roms*)data;

    static const char *str[] = { "Select ROM to boot:", "No ROMs in this location!" };
    t->title_text->head.x = center_x(0, fb_width, SIZE_BIG, str[empty]);
    t->title_text->text = realloc(t->title_text->text, strlen(str[empty])+1);
    strcpy(t->title_text->text, str[empty]);

    if(t->boot_btn)
        button_enable(t->boot_btn, !empty);

    if(empty && !t->usb_text)
    {
        static const char *txt = "This list is refreshed automagically,\njust plug in the USB drive and  wait.";
        int x = (fb_width/2 - (37*ISO_CHAR_WIDTH*SIZE_NORMAL)/2);
        int y = center_y(HEADER_HEIGHT, t->list->h, SIZE_NORMAL);
        t->usb_text = fb_add_text(x, y, WHITE, SIZE_NORMAL, txt);
        list_add(t->usb_text, &t->ui_elements);

        x = (fb_width/2) - (PROGDOTS_W/2);
        t->usb_prog = progdots_create(x, y+100);
    }
    else if(!empty && t->usb_text)
    {
        progdots_destroy(t->usb_prog);
        t->usb_prog = NULL;

        list_rm(t->usb_text, &t->ui_elements, &fb_remove_item);
        t->usb_text = NULL;
    }
}

#define MISCBTN_W 265
#define MISCBTN_H 100

#define CLRBTN_W 50
#define CLRBTN_B 10
#define CLRBTN_TOTAL (CLRBTN_W+CLRBTN_B)
#define CLRBTN_Y 1150

typedef struct 
{
    button **buttons;
    void **ui_elements;
} tab_misc;

void *multirom_ui_tab_misc_init(void)
{
    tab_misc *t = malloc(sizeof(tab_misc));
    memset(t, 0, sizeof(tab_misc));

    int x = fb_width/2 - MISCBTN_W/2;
    int y = 270;

    button *b = malloc(sizeof(button));
    memset(b, 0, sizeof(button));
    b->x = x;
    b->y = y;
    b->w = MISCBTN_W;
    b->h = MISCBTN_H;
    b->clicked = &multirom_ui_tab_misc_copy_log;
    button_init_ui(b, "Copy log to /sdcard", SIZE_BIG);
    list_add(b, &t->buttons);

    y += MISCBTN_H+70;

    static const char *texts[] = 
    {
        "Reboot",               // 0
        "Reboot to recovery",   // 1
        "Reboot to bootloader", // 2
        "Shutdown",             // 3
        NULL
    };

    static const int exit_codes[] = {
        UI_EXIT_REBOOT, UI_EXIT_REBOOT_RECOVERY,
        UI_EXIT_REBOOT_BOOTLOADER, UI_EXIT_SHUTDOWN
    };

    int i;
    for(i = 0; texts[i]; ++i)
    {
        b = malloc(sizeof(button));
        memset(b, 0, sizeof(button));
        b->x = x;
        b->y = y;
        b->w = MISCBTN_W;
        b->h = MISCBTN_H;
        b->action = exit_codes[i];
        b->clicked = &multirom_ui_reboot_btn;
        button_init_ui(b, texts[i], SIZE_BIG);
        list_add(b, &t->buttons);

        y += MISCBTN_H+20;
        if(i == 2)
            y += 50;
    }

    fb_text *text = fb_add_text(0, fb_height-16, WHITE, SIZE_SMALL, "MultiROM v%d with trampoline v%d.",
                               VERSION_MULTIROM, multirom_get_trampoline_ver());
    list_add(text, &t->ui_elements);

    char bat_text[16];
    sprintf(bat_text, "Battery: %d%%", multirom_get_battery());
    text = fb_add_text_long(fb_width-strlen(bat_text)*8, fb_height-16, WHITE, SIZE_SMALL, bat_text);
    list_add(text, &t->ui_elements);

    x = fb_width/2 - (CLRS_MAX*CLRBTN_TOTAL)/2;
    uint32_t p, s;
    fb_rect *r;
    for(i = 0; i < CLRS_MAX; ++i)
    {
        multirom_ui_setup_colors(i, &p, &s);

        if(i == mrom_status->colors)
        {
            r = fb_add_rect(x, CLRBTN_Y, CLRBTN_TOTAL, CLRBTN_TOTAL, WHITE);
            list_add(r, &t->ui_elements);
        }

        r = fb_add_rect(x+CLRBTN_B/2, CLRBTN_Y+CLRBTN_B/2, CLRBTN_W, CLRBTN_W, p);
        list_add(r, &t->ui_elements);

        b = malloc(sizeof(button));
        memset(b, 0, sizeof(button));
        b->x = x;
        b->y = CLRBTN_Y;
        b->w = CLRBTN_TOTAL;
        b->h = CLRBTN_TOTAL;
        b->action = i;
        b->clicked = &multirom_ui_tab_misc_change_clr;
        button_init_ui(b, NULL, 0);
        list_add(b, &t->buttons);

        x += CLRBTN_TOTAL;
    }
    return t;
}

void multirom_ui_tab_misc_destroy(void *data)
{
    tab_misc *t = (tab_misc*)data;

    list_clear(&t->ui_elements, &fb_remove_item);
    list_clear(&t->buttons, &button_destroy);

    free(t);
}

void multirom_ui_tab_misc_change_clr(int clr)
{
    if((loop_act & LOOP_CHANGE_CLR) || mrom_status->colors == clr)
        return;

    pthread_mutex_lock(&exit_code_mutex);
    mrom_status->colors = clr;
    loop_act |= LOOP_CHANGE_CLR;
    pthread_mutex_unlock(&exit_code_mutex);
}

void multirom_ui_reboot_btn(int action)
{
    pthread_mutex_lock(&exit_code_mutex);
    exit_ui_code = action;
    pthread_mutex_unlock(&exit_code_mutex);
}

void multirom_ui_tab_misc_copy_log(int action)
{
    multirom_dump_status(mrom_status);

    int res = multirom_copy_log(NULL);

    static const char *text[] = { "Failed to copy log to sdcard!", "Successfully copied error log!" };

    active_msgbox = fb_create_msgbox(550, 260, res ? DRED : CLR_PRIMARY);
    fb_msgbox_add_text(-1, 50, SIZE_NORMAL, (char*)text[res+1]);
    if(res == 0)
        fb_msgbox_add_text(-1, -1, SIZE_NORMAL, "/sdcard/multirom/error.txt");
    fb_msgbox_add_text(-1, active_msgbox->h-60, SIZE_NORMAL, "Touch anywhere to close");

    fb_draw();
    fb_freeze(1);
    set_touch_handlers_mode(HANDLERS_ALL);
}
