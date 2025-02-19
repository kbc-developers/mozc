// Copyright 2010-2016, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

package org.mozc.android.inputmethod.japanese.preference;

import static android.test.MoreAsserts.assertEmpty;

import org.mozc.android.inputmethod.japanese.testing.ActivityInstrumentationTestCase2WithMock;
import org.mozc.android.inputmethod.japanese.testing.ApiLevel;

import android.preference.PreferenceActivity.Header;
import android.test.suitebuilder.annotation.SmallTest;

import java.util.ArrayList;
import java.util.List;

/**
 */
@ApiLevel(11)
public class MozcFragmentPreferenceActivityTest
    extends ActivityInstrumentationTestCase2WithMock<MozcFragmentPreferenceActivity> {
  public MozcFragmentPreferenceActivityTest() {
    super(MozcFragmentPreferenceActivity.class);
  }

  @SmallTest
  public void testLoadHeaders() {
    MozcFragmentPreferenceActivity activity = getActivity();
    List<Header> target = new ArrayList<Header>();

    // Multi-pane
    activity.loadHeaders(target, true);
    assertTrue(target.size() > 1);

    // Single-pane
    target.clear();
    activity.loadHeaders(target, false);
    assertEmpty(target);
  }
}
