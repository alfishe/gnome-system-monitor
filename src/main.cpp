/* Procman
 * Copyright (C) 2001 Kevin Vandersloot
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include <adwaita.h>
#include <locale.h>
#include <glib/gi18n.h>

#include "application.h"


int
main (int   argc,
      char *argv[])
{
  bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  setlocale (LC_ALL, "");

  /* The UI is flat 2D content whose only animation is blitting cached graph
   * layers, which the cairo renderer does with damage tracking at about the
   * same cost as the GPU renderers, without setting up GL/Vulkan, and it is
   * several times cheaper where OpenGL is software rendered (remote and
   * headless sessions). An explicit GSK_RENDERER still takes precedence. */
  g_setenv ("GSK_RENDERER", "cairo", FALSE);

  adw_init ();

  Glib::RefPtr<GsmApplication> application = GsmApplication::get ();

  return application->run (argc, argv);
}
