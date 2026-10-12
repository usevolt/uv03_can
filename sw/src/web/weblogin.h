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

#ifndef UVCAN_WEB_WEBLOGIN_H_
#define UVCAN_WEB_WEBLOGIN_H_


/// @brief: Takes the account the page's login form was given (web/index.html,
/// Module.uvLogin) into the stored credentials, which is what the Settings
/// tab's account connect logs in with on start-up. The page's own copy of the
/// password is dropped once it has been read.
void weblogin_apply(void);


#endif /* UVCAN_WEB_WEBLOGIN_H_ */
