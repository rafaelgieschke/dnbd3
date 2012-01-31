/*
 * This file is part of the Distributed Network Block Device 3
 *
 * Copyright(c) 2011-2012 Johann Latocha <johann@latocha.de>
 *
 * This file may be licensed under the terms of of the
 * GNU General Public License Version 2 (the ``GPL'').
 *
 * Software distributed under the License is distributed
 * on an ``AS IS'' basis, WITHOUT WARRANTY OF ANY KIND, either
 * express or implied. See the GPL for the specific language
 * governing rights and limitations.
 *
 * You should have received a copy of the GPL along with this
 * program. If not, go to http://www.gnu.org/licenses/gpl.html
 * or write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include <unistd.h>
#include <sys/types.h>

#include "../config.h"

#ifndef UTILS_H_
#define UTILS_H_

pid_t dnbd3_read_pid_file();
void dnbd3_write_pid_file(pid_t pid);
void dnbd3_delete_pid_file();

void dnbd3_load_config(char* config_file_name);
void dnbd3_reload_config(char* config_file_name);

void dnbd3_send_signal(int signum);

#endif /* UTILS_H_ */
