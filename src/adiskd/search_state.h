#ifndef _SEARCH_STATE_H_
#define _SEARCH_STATE_H_


#define DEV_FLAG_RUNNING                0x01
#define DEV_FLAG_COMPLETE               0x02


typedef struct search_state {
	void *			comm_cookie;
        pthread_t               thread_id;
        unsigned int            flags;
        odisk_state_t *         ostate;
        int                     ver_no;
        ring_data_t *           control_ops;
} search_state_t;



/*
 * Function prototypes for the search functions.
 */

extern int search_new_conn(void *cookie, void **app_cookie);
extern int search_close_conn(void *app_cookie);
extern int search_start(void *app_cookie, int gen_num);
extern int search_stop(void *app_cookie, int gen_num);
extern int search_set_searchlet(void *app_cookie, int gen_num,
		char *filter, char *spec);
extern int search_set_list(void *app_cookie, int gen_num);
extern int search_term(void *app_cookie, int gen_num);
extern int search_get_stats(void *app_cookie, int gen_num);
extern int search_release_obj(void *app_cookie, obj_data_t *obj);
extern int search_get_char(void *app_cookie, int gen_num);


#endif	/* ifndef _SEARCH_STATE_H_ */