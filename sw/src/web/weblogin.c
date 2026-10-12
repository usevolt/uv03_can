/*
 * This file is part of the uv_hal distribution (www.usevolt.fi).
 * Copyright (c) 2026 Usevolt Oy.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#if CONFIG_TARGET_WEB

#include "weblogin.h"
#include "credentials.h"
#include <emscripten.h>
#include <string.h>


// Copies field *key* of Module.uvLogin into *dest*, "" when there is none
EM_JS(void, js_login_field, (const char *key, char *dest, int len), {
	const login = Module.uvLogin || {};
	const v = login[UTF8ToString(key)];
	stringToUTF8((typeof v === 'string') ? v : "", dest, len);
});

EM_JS(void, js_login_forget_password, (void), {
	if (Module.uvLogin) {
		delete Module.uvLogin.password;
	}
});


void weblogin_apply(void) {
	char v[CREDENTIALS_MAX];

	js_login_field("url", v, sizeof(v));
	if (v[0] != '\0') {
		credentials_set_url(v);
	}
	js_login_field("fleetUrl", v, sizeof(v));
	if (v[0] != '\0') {
		credentials_fleet_set_url(v);
	}
	js_login_field("username", v, sizeof(v));
	credentials_set_username(v);
	js_login_field("password", v, sizeof(v));
	credentials_set_password(v);
	memset(v, 0, sizeof(v));
	js_login_forget_password();
}

#endif
