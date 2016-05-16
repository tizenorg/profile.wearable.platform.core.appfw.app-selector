/*
 * Copyright 2012  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://floralicense.org/license

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <app.h>
#include <stdio.h>
#include <appcore-efl.h>
#include <aul.h>
#include <appsvc.h>
#include <bundle_internal.h>
#include <bundle.h>

#include "app-selector.h"
#include "app-selector-view.h"

#define WIN_PROP_NAME "APP_SELECTOR"

#ifndef EXPORT_API
#define EXPORT_API __attribute__ ((visibility("default")))
#endif

extern int app_send_cmd_with_noreply(int pid, int cmd, bundle *kb);
extern void popup_update_by_lang_changed_sig(struct appdata *ad);
extern int app_control_export_as_bundle(app_control_h app_control, bundle **data);

bool enable_pause_terminate = true;
Ecore_Timer *exit_timer;

int is_reset = 0;

static void __win_del(void *data, Evas_Object * obj, void *event)
{
	ui_app_exit();
}

static Eina_Bool __exit_timer_cb(void *data)
{
	_D("selected app is not launched successfully. abnormal case. selector popup will be terminated by force.");

	ui_app_exit();

	return ECORE_CALLBACK_CANCEL;
}

void _tasks_related_launching_app(void *data)
{
	/* app terminate timer for case that abnormal case like selected app launch fail */
	exit_timer = ecore_timer_add(3, __exit_timer_cb, NULL);

	enable_pause_terminate = true;
}

static Evas_Object *__create_win(const char *name, struct appdata *ad)
{
	Evas_Object *eo;
	Evas *e;
	Ecore_Evas *ee;
	Evas_Object *bg;

	eo = elm_win_add(NULL, name, ELM_WIN_BASIC);
	if (eo) {
		elm_win_title_set(eo, name);
		elm_win_borderless_set(eo, EINA_TRUE);
		elm_win_alpha_set(eo, EINA_TRUE);

		evas_object_smart_callback_add(eo, "delete,request",
						__win_del, NULL);

		elm_win_screen_size_get(eo, NULL, NULL, &(ad->root_w), &(ad->root_h));
		evas_object_resize(eo, ad->root_w, ad->root_h);

		if (elm_win_wm_rotation_supported_get(ad->win)) {
			int rots[4] = { 0, 90, 180, 270 };
			elm_win_wm_rotation_available_rotations_set(ad->win, (const int*)(&rots), 4);
		} else {
			_E("win rotation no supported");
		}

		e = evas_object_evas_get(eo);
		if (!e)
			goto error;

		ee = ecore_evas_ecore_evas_get(e);
		if (!ee)
			goto error;

		bg = elm_bg_add(eo);
		evas_object_size_hint_weight_set(bg, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
		elm_win_resize_object_add(eo, bg);
		evas_object_show(bg);
	}

	return eo;
error:
	if (eo)
		evas_object_del(eo);

	return NULL;
}

static Evas_Object *__create_layout_main(Evas_Object * parent)
{
	Evas_Object *layout;

	if (!parent)
		return NULL;

	layout = elm_layout_add(parent);
	if (!layout)
		return NULL;

	elm_layout_theme_set(layout, "layout", "application", "default");
	evas_object_size_hint_weight_set(layout, EVAS_HINT_EXPAND,
					 EVAS_HINT_EXPAND);

	edje_object_signal_emit(_EDJ(layout), "elm,state,show,content", "elm");

	evas_object_show(layout);

	return layout;
}

static bool __app_create(void *data)
{
	struct appdata *ad = (struct appdata *)data;
	Evas_Object *win;
	Evas_Object *ly;
	int r;

	/* create window */
	win = __create_win(PACKAGE, ad);
	if (win == NULL)
		return -1;
	ad->win = win;

	/* Base Layout */
	ly = __create_layout_main(ad->win);
	if (!ly)
		return -1;

	ad->layout = ly;

	edje_object_signal_emit(_EDJ(ly), "elm,bg,show,transparent", "elm");

	if (elm_win_wm_rotation_supported_get(ad->win)) {
		int rots[4] = { 0, 90, 180, 270 };
		elm_win_wm_rotation_available_rotations_set(ad->win, (const int*)(&rots), 4);
	} else {
		_E("win rotation no supported");
	}

	evas_object_show(win);

	/* init internationalization */
	r = appcore_set_i18n(PACKAGE, LOCALEDIR);
	if (r)
		return false;

	return true;
}

static int __app_init_with_bundle(void *data)
{
	struct appdata *ad = data;

	load_app_select_popup(ad);

	return 0;
}

static void __app_terminate(void *data)
{
	struct appdata *ad = data;
	const char *val_noti = NULL;
	const char *val_launch = NULL;
	int ret;

	_D("app_terminate");

	if (exit_timer) {
		ecore_timer_del(exit_timer);
		exit_timer = NULL;
	}

	val_noti = bundle_get_val(ad->kb, "__APP_SVC_CALLER_NOTI__");
	val_launch = bundle_get_val(ad->kb, "__APP_SVC_START_INFO__");

	/* For native C++ app */
	if (val_noti) {
		/* Cancel case. START_INFO is set when app is launched successful */
		if (aul_app_is_running(val_noti)) {
			if (!val_launch)
				bundle_add(ad->kb, "__APP_SVC_START_INFO__", "c");

			ret = aul_launch_app(val_noti, ad->kb);

			if (ret < AUL_R_OK)
				_E("noti for natie app is failed(%d)", ret);
		}
	/* For C app */
	} else {
		/* Cancel case. START_INFO is set when app is launched successful */
		if (!val_launch) {
			do {
				bundle *kb;
				int ret_val;
				const char *pid = NULL;
				char callee_pid[20] = {0,};

				pid = bundle_get_val(ad->kb, AUL_K_CALLER_PID);
				if (pid == NULL) {
					_E("get CALLER PID ERROR");
					break;
				} else {
					_D("CALLER PID(%s)", pid);
				}

				kb = bundle_create();
				if (kb == NULL) {
					_E("bundle create error");
					break;
				}
				bundle_add(kb, AUL_K_SEND_RESULT, "1");
				bundle_add(kb, AUL_K_CALLER_PID, pid);

				snprintf(callee_pid, 20, "%d", getpid());
				bundle_add(kb, AUL_K_CALLEE_PID, (const char*)callee_pid);

				ret_val = app_send_cmd_with_noreply(-2, 7, kb); /* 7 is APP_CANCEL */
				if (ret_val != AUL_R_OK)
					_E("app_send_cmd error(%d)", ret_val);

				bundle_free(kb);
			} while (0);
		}
	}

	clear_list_info(ad);

	if (ad->layout) {
		evas_object_del(ad->layout);
		ad->layout = NULL;
	}

	if (ad->popup) {
		evas_object_del(ad->popup);
		ad->popup = NULL;
	}

	if (ad->win) {
		evas_object_del(ad->win);
		ad->win = NULL;
	}

	if (ad->kb) {
		bundle_free(ad->kb);
		ad->kb = NULL;
	}

	return;
}

static void __app_pause(void *data)
{
	_D("app_pause");

	if (enable_pause_terminate) {
		_D("pause_terminate is true. app will be terminated.");
		ui_app_exit();
	}

	return;
}

static void __app_resume(void *data)
{
	_D("app_resume");

	return;
}

static void __app_control(app_control_h app_control, void *data)
{
	_D("app_control");

	struct appdata *ad = (struct appdata *)data;
	const char *str = NULL;
	int ret = 0;

	if (ad->kb) {
		bundle_free(ad->kb);
		ad->kb = NULL;
	}

	app_control_export_as_bundle(app_control, &(ad->kb));

	ad->window_id = (char *)bundle_get_val(ad->kb, "__APP_SVC_K_WIN_ID__");
	_D("window id is %s", ad->window_id);

	str = (char *)bundle_get_val(ad->kb, AUL_K_CALLER_PID);
	_D("caller pid is %s", str);

	if (str)
		ad->caller_pid = atoi(str);
	else
		ad->caller_pid = -1;

	app_control_get_operation(app_control, &ad->control_op);
	app_control_get_mime(app_control, &ad->control_mime);
	app_control_get_uri(app_control, &ad->control_uri);

	_D("control_op is %s", ad->control_op);
	_D("control_uri is %s", ad->control_uri);
	_D("control_mime is %s", ad->control_mime);

	ret = bundle_get_type(ad->kb, APP_SVC_K_SELECTOR_EXTRA_LIST);

	if (ret != BUNDLE_TYPE_NONE) {
		if (ret & BUNDLE_TYPE_ARRAY) {
			ad->extra_list = bundle_get_str_array(ad->kb, APP_SVC_K_SELECTOR_EXTRA_LIST, &ad->extra_list_cnt);
			_D("extra list cnt : %d", ad->extra_list_cnt);
		} else {
			str = bundle_get_val(ad->kb, APP_SVC_K_SELECTOR_EXTRA_LIST);
			if (str) {
				ad->extra_list = (const char**)(&str);
				ad->extra_list_cnt = 1;
			}
		}
	}

	if (!ad->control_op) {
		load_info_popup(ad, dgettext("sys_string", "IDS_COM_BODY_NO_APPLICATIONS_CAN_PERFORM_THIS_ACTION"));
		return;
	}

	/*
	   If AUL_K_ARGV0 is not NULL, the situation is launching(fork & exec).
	   else the situation is being received a reset event(old relaunch evet)
	 */
	if (is_reset == 0) {
		__app_init_with_bundle(data);
		is_reset = 1;
		evas_object_show(ad->win);
	} else {
		update_app_list(data);	/*(reset event) */
		elm_win_activate(ad->win);
	}

	return;
}

static void __orientation_changed_cb(app_event_info_h event_info, void *user_data)
{
	app_device_orientation_e e;
	app_event_get_device_orientation(event_info, &e);
	_D("orientation changed :%d", e);
}

static void __language_changed_cb(app_event_info_h event_info, void *user_data)
{
	char *lang = NULL;
	int ret;

	_D("language changed");

	ret = app_event_get_language(event_info, &lang);
	if (ret != APP_ERROR_NONE) {
		_E("Fail to get string value(%d)", ret);
		return;
	}

	_D("lang: %s", lang);

	if (!lang)
		return;

	elm_language_set(lang);

	free(lang);

	popup_update_by_lang_changed_sig(user_data);
}

EXPORT_API int main(int argc, char *argv[])
{
	int ret = 0;

	struct appdata ad;
	app_event_handler_h orientation_h;
	app_event_handler_h language_h;


	memset(&ad, 0, sizeof(struct appdata));

	ui_app_lifecycle_callback_s lifecycle_callback = {0,};

	lifecycle_callback.create = __app_create;
	lifecycle_callback.terminate = __app_terminate;
	lifecycle_callback.pause = __app_pause;
	lifecycle_callback.resume = __app_resume;
	lifecycle_callback.app_control = __app_control;

	ui_app_add_event_handler(&orientation_h, APP_EVENT_DEVICE_ORIENTATION_CHANGED, __orientation_changed_cb, &ad);
	ui_app_add_event_handler(&language_h, APP_EVENT_LANGUAGE_CHANGED, __language_changed_cb, &ad);

	ret = ui_app_main(argc, argv, &lifecycle_callback, &ad);
	if (ret != APP_ERROR_NONE)
		_E("app_main() is failed. err = %d", ret);

	return ret;
}
