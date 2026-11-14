;;; clawtilla-link.el --- clawtilla: links for org-mode  -*- lexical-binding: t; -*-

;; Copyright (C) 2026
;; SPDX-License-Identifier: AGPL-3.0-or-later
;;
;; This file is part of clawtilla.
;;
;; This program is free software: you can redistribute it and/or modify
;; it under the terms of the GNU Affero General Public License as published
;; by the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.
;;
;; This program is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU Affero General Public License for more details.
;;
;; You should have received a copy of the GNU Affero General Public License
;; along with this program.  If not, see <https://www.gnu.org/licenses/>.

;; Keywords: hypermedia, outlines, comm
;; Package-Requires: ((emacs "27.1"))

;;; Commentary:

;; An org-mode link type for clawtilla conversations:
;;
;;   [[clawtilla:chief][the chief's chat]]
;;   [[clawtilla:standup]]
;;   [[clawtilla:dm:chief:lead][what chief asked lead]]
;;
;; Following one runs `clawtilla-gtk --open <room>', which hands the room
;; to the window already running (the GTK client is single-instance) or
;; starts one on it.  The target is an agent id (its chat with you), a
;; group room id, or a direct room between two agents; the client checks
;; it against the daemon and says so in the window when nothing by that
;; name exists.
;;
;; Set `clawtilla-link-web-url' to open the web client's `/r/<room>'
;; instead, in `browse-url' -- the right choice on a machine with no
;; desktop client, or for links exported to HTML.
;;
;; Install: put this file on `load-path' and
;;
;;   (require 'clawtilla-link)
;;
;; or, in Doom, `(load! "clawtilla-link")'.  See docs/clients.org,
;; "Open-room links".

;;; Code:

(require 'url-util)

(declare-function org-link-set-parameters "ol" (type &rest parameters))

(defgroup clawtilla-link nil
  "Links to clawtilla conversations from org-mode."
  :group 'org-link
  :prefix "clawtilla-link-")

(defcustom clawtilla-link-command "clawtilla-gtk"
  "The desktop client a `clawtilla:' link runs.
It is given `clawtilla-link-arguments' and then the room.  Any client
that takes `--open <room>' works: `clawtilla-lrg' does too, though it is
not single-instance and so starts a new window each time."
  :type 'string
  :group 'clawtilla-link)

(defcustom clawtilla-link-arguments '("--open")
  "Arguments placed before the room on `clawtilla-link-command'.
Add a connection here to open links on another daemon, for example
\\='(\"--profile\" \"workstation\" \"--open\")."
  :type '(repeat string)
  :group 'clawtilla-link)

(defcustom clawtilla-link-web-url nil
  "The web client's address, or nil to run `clawtilla-link-command'.
When set (for example \"http://127.0.0.1:8790\"), a link opens
`<url>/r/<room>' with `browse-url', and links exported to HTML point
there too."
  :type '(choice (const :tag "Run the desktop client" nil)
                 (string :tag "Web client URL"))
  :group 'clawtilla-link)

(defconst clawtilla-link--room-regexp "\\`[a-z0-9._:-]+\\'"
  "What a room id may be spelled with.
The same characters `clawt_open_target_normalize()' accepts, checked
here as well so a mistyped link is reported in the echo area rather
than passed to a process.")

(defun clawtilla-link--room (path)
  "Return the room PATH names, or signal a `user-error'.
A leading `//' and a trailing `/' are dropped, so a link written as
`clawtilla://chief/' names `chief' too."
  (let ((room (string-trim (or path "") "/*" "/*")))
    (unless (string-match-p clawtilla-link--room-regexp room)
      (user-error "clawtilla: %S is not an agent or room id (lower-case letters, digits, `-', `_', `.' and `:')"
                  path))
    room))

(defun clawtilla-link--web-target (room)
  "Return the web client's URL for ROOM under `clawtilla-link-web-url'."
  (concat (string-trim-right clawtilla-link-web-url "/+")
          "/r/" (url-hexify-string room)))

(defun clawtilla-link-open (path &optional _arg)
  "Open the clawtilla conversation PATH names.
With `clawtilla-link-web-url' set, in the browser; otherwise by
running `clawtilla-link-command' with `clawtilla-link-arguments'
and the room.  The process is started directly, never through a
shell, and its output goes to the buffer ` *clawtilla-link*'."
  (let ((room (clawtilla-link--room path)))
    (if clawtilla-link-web-url
        (browse-url (clawtilla-link--web-target room))
      (unless (executable-find clawtilla-link-command)
        (user-error "clawtilla: %s is not on PATH (see `clawtilla-link-command')"
                    clawtilla-link-command))
      (make-process
       :name "clawtilla-link"
       :buffer (get-buffer-create " *clawtilla-link*")
       :command (append (list clawtilla-link-command)
                        clawtilla-link-arguments
                        (list room))
       :noquery t
       :sentinel #'clawtilla-link--sentinel))))

(defun clawtilla-link--sentinel (process _event)
  "Report PROCESS failing to open its room, in the echo area.
A refused spelling exits non-zero with the reason on stderr; an unknown
room is reported in the window instead, where the fleet is."
  (when (and (memq (process-status process) '(exit signal))
             (/= (process-exit-status process) 0))
    (message "clawtilla: %s exited %d; see the buffer ` *clawtilla-link*'"
             (car (process-command process))
             (process-exit-status process))))

(defun clawtilla-link-export (path description backend &optional _info)
  "Export a `clawtilla:' link to PATH with DESCRIPTION for BACKEND.
HTML gets a real link when `clawtilla-link-web-url' is set; every
other case is the description, or the room, as plain text."
  (let ((room (ignore-errors (clawtilla-link--room path)))
        (text (or description path)))
    (cond
     ((and room clawtilla-link-web-url (eq backend 'html))
      (format "<a href=\"%s\">%s</a>"
              (clawtilla-link--web-target room)
              (url-insert-entities-in-string text)))
     (t text))))

(with-eval-after-load 'ol
  (org-link-set-parameters "clawtilla"
                           :follow #'clawtilla-link-open
                           :export #'clawtilla-link-export))

(provide 'clawtilla-link)

;;; clawtilla-link.el ends here
