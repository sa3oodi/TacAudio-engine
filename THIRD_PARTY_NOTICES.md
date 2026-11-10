# Third-party notices

TacAudio includes or downloads the following third-party components.

## SADIE II HRTF database (data, shipped with the installer: D1 and D2)

University of York, SADIE II database, https://www.york.ac.uk/sadie-project/ —
Zenodo record 12092466. Licensed under the **Apache License 2.0**; the full licence text is
installed as `hrtf/sadie2/LICENSE.txt`. The SOFA files are redistributed unmodified; TacAudio
equalises them at load time.

## MIT KEMAR HRTF (data, shipped as `hrtf/MIT_KEMAR_normal_pinna.sofa`)

Bill Gardner and Keith Martin, MIT Media Lab, "HRTF Measurements of a KEMAR Dummy-Head
Microphone" (1994). The measurements may be used freely provided the source is acknowledged.
The SOFA conversion is the one distributed with libmysofa.

## libmysofa (statically linked)

Copyright (c) 2016-2017, Symonics GmbH, Christian Hoene. All rights reserved.
BSD 3-Clause licence: redistribution and use in source and binary forms, with or without
modification, are permitted provided that (1) redistributions of source code retain the above
copyright notice, this list of conditions and the following disclaimer; (2) redistributions in
binary form reproduce the above copyright notice, this list of conditions and the following
disclaimer in the documentation and/or other materials provided with the distribution; (3) the
name of the author may not be used to endorse or promote products derived from this software
without specific prior written permission. THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. https://github.com/hoene/libmysofa

libmysofa includes the Speex resampler (BSD-style licence, Copyright (C) 2007-2008 Jean-Marc Valin,
Copyright (C) 2008 Thorvald Natvig).

## zlib (statically linked)

(C) 1995-2026 Jean-loup Gailly and Mark Adler. zlib licence: this software is provided 'as-is',
without any express or implied warranty. Permission is granted to anyone to use this software for
any purpose, including commercial applications, and to alter it and redistribute it freely,
subject to the restrictions in the zlib licence. https://zlib.net

## Dear ImGui (statically linked)

Copyright (c) 2014-2026 Omar Cornut. MIT licence: permission is hereby granted, free of charge,
to any person obtaining a copy of this software and associated documentation files, to deal in
the software without restriction, subject to including the copyright notice and permission
notice in all copies or substantial portions of the software. THE SOFTWARE IS PROVIDED "AS IS",
WITHOUT WARRANTY OF ANY KIND. https://github.com/ocornut/imgui

## Fonts

Segoe UI and Segoe Fluent Icons / Segoe MDL2 Assets are Windows system fonts. TacAudio loads
them from the Windows installation at run time and does not redistribute them.

## Not included

VB-CABLE (VB-Audio Software) is a separate product that users download and install themselves
from https://vb-audio.com/Cable/. It is not part of TacAudio and not redistributed.
