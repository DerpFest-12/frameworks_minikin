/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define VERBOSE_DEBUG 0

#define LOG_TAG "Minikin"

#include <limits>

#include <log/log.h>

#include "LayoutUtils.h"
#include <minikin/Layout.h>
#include <minikin/LineBreaker.h>

using std::vector;

namespace minikin {

constexpr uint16_t CHAR_TAB = 0x0009;
constexpr uint16_t CHAR_NBSP = 0x00A0;

// Large scores in a hierarchy; we prefer desperate breaks to an overfull line. All these
// constants are larger than any reasonable actual width score.
const float SCORE_INFTY = std::numeric_limits<float>::max();
const float SCORE_OVERFULL = 1e12f;
const float SCORE_DESPERATE = 1e10f;

// Multiplier for hyphen penalty on last line.
const float LAST_LINE_PENALTY_MULTIPLIER = 4.0f;
// Penalty assigned to each line break (to try to minimize number of lines)
// TODO: when we implement full justification (so spaces can shrink and stretch), this is
// probably not the most appropriate method.
const float LINE_PENALTY_MULTIPLIER = 2.0f;

// Penalty assigned to shrinking the whitepsace.
const float SHRINK_PENALTY_MULTIPLIER = 4.0f;

// Very long words trigger O(n^2) behavior in hyphenation, so we disable hyphenation for
// unreasonably long words. This is somewhat of a heuristic because extremely long words
// are possible in some languages. This does mean that very long real words can get
// broken by desperate breaks, with no hyphens.
const size_t LONGEST_HYPHENATED_WORD = 45;

// When the text buffer is within this limit, capacity of vectors is retained at finish(),
// to avoid allocation.
const size_t MAX_TEXT_BUF_RETAIN = 32678;

// Maximum amount that spaces can shrink, in justified text.
const float SHRINKABILITY = 1.0 / 3.0;

void LineBreaker::setLocales(const char* locales, const std::vector<Hyphenator*>& hyphenators) {
    bool goodLocaleFound = false;
    const ssize_t numLocales = hyphenators.size();
    // For now, we ignore all locales except the first valid one.
    // TODO: Support selecting the locale based on the script of the text.
    const char* localeStart = locales;
    for (ssize_t i = 0; i < numLocales - 1; i++) { // Loop over all locales, except the last one.
        const char* localeEnd = strchr(localeStart, ',');
        const size_t localeNameLength = localeEnd - localeStart;
        char localeName[localeNameLength + 1];
        strncpy(localeName, localeStart, localeNameLength);
        localeName[localeNameLength] = '\0';
        mLocale = icu::Locale::createFromName(localeName);
        goodLocaleFound = !mLocale.isBogus();
        if (goodLocaleFound) {
            mHyphenator = hyphenators[i];
            break;
        } else {
            localeStart = localeEnd + 1;
        }
    }
    if (!goodLocaleFound) { // Try the last locale.
        mLocale = icu::Locale::createFromName(localeStart);
        if (mLocale.isBogus()) {
            // No good locale.
            mLocale = icu::Locale::getRoot();
            mHyphenator = nullptr;
        } else {
            mHyphenator = numLocales == 0 ? nullptr : hyphenators[numLocales - 1];
        }
    }
    mWordBreaker.setLocale(mLocale);
}

void LineBreaker::setText() {
    mWordBreaker.setText(mTextBuf.data(), mTextBuf.size());

    // handle initial break here because addStyleRun may never be called
    mWordBreaker.next();
    mCandidates.clear();
    Candidate cand = {
            0, 0, 0.0, 0.0, 0.0, 0.0, 0, 0, {0.0, 0.0, 0.0}, HyphenationType::DONT_BREAK};
    mCandidates.push_back(cand);

    // reset greedy breaker state
    mBreaks.clear();
    mWidths.clear();
    mAscents.clear();
    mDescents.clear();
    mFlags.clear();
    mLastBreak = 0;
    mBestBreak = 0;
    mBestScore = SCORE_INFTY;
    mPreBreak = 0;
    mLastHyphenation = HyphenEdit::NO_EDIT;
    mFirstTabIndex = INT_MAX;
    mSpaceCount = 0;
}

// This function determines whether a character is a space that disappears at end of line.
// It is the Unicode set: [[:General_Category=Space_Separator:]-[:Line_Break=Glue:]],
// plus '\n'.
// Note: all such characters are in the BMP, so it's ok to use code units for this.
static bool isLineEndSpace(uint16_t c) {
    return c == '\n' || c == ' ' || c == 0x1680 || (0x2000 <= c && c <= 0x200A && c != 0x2007) ||
            c == 0x205F || c == 0x3000;
}

// Hyphenates a string potentially containing non-breaking spaces. The result would be saved
// in mHyphBuf.
void LineBreaker::hyphenate(const uint16_t* str, size_t len) {
    mHyphBuf.clear();
    mHyphBuf.reserve(len);

    // A word here is any consecutive string of non-NBSP characters.
    bool inWord = false;
    size_t wordStart = 0; // The initial value will never be accessed, but just in case.
    for (size_t i = 0; i <= len; i++) {
        if (i == len || str[i] == CHAR_NBSP) {
            if (inWord) {
                // A word just ended. Hyphenate it.
                const size_t wordLen = i - wordStart;
                if (wordLen <= LONGEST_HYPHENATED_WORD) {
                    if (wordStart == 0) {
                        // The string starts with a word. Use mHyphBuf directly.
                        mHyphenator->hyphenate(&mHyphBuf, str, wordLen, mLocale);
                    } else {
                        std::vector<HyphenationType> wordVec;
                        mHyphenator->hyphenate(&wordVec, str + wordStart, wordLen, mLocale);
                        mHyphBuf.insert(mHyphBuf.end(), wordVec.cbegin(), wordVec.cend());
                    }
                } else { // Word is too long. Inefficient to hyphenate.
                    mHyphBuf.insert(mHyphBuf.end(), wordLen, HyphenationType::DONT_BREAK);
                }
                inWord = false;
            }
            if (i < len) {
                // Insert one DONT_BREAK for the NBSP.
                mHyphBuf.push_back(HyphenationType::DONT_BREAK);
            }
        } else if (!inWord) {
            inWord = true;
            wordStart = i;
        }
    }
}

// Ordinarily, this method measures the text in the range given. However, when paint
// is nullptr, it assumes the character widths and extents have already been calculated and stored
// in the mCharWidths and mCharExtents buffers.
//
// This method finds the candidate word breaks (using the ICU break iterator) and sends them
// to addCandidate.
float LineBreaker::addStyleRun(MinikinPaint* paint, const std::shared_ptr<FontCollection>& typeface,
        FontStyle style, size_t start, size_t end, bool isRtl) {
    float width = 0.0f;
    int bidiFlags = isRtl ? kBidi_Force_RTL : kBidi_Force_LTR;

    float hyphenPenalty = 0.0;
    if (paint != nullptr) {
        width = Layout::measureText(mTextBuf.data(), start, end - start, mTextBuf.size(), bidiFlags,
                style, *paint, typeface, mCharWidths.data() + start, mCharExtents.data() + start);

        // a heuristic that seems to perform well
        hyphenPenalty = 0.5 * paint->size * paint->scaleX * mLineWidthDelegate->getLineWidth(0);
        if (mHyphenationFrequency == kHyphenationFrequency_Normal) {
            hyphenPenalty *= 4.0; // TODO: Replace with a better value after some testing
        }

        if (mJustified) {
            // Make hyphenation more aggressive for fully justified text (so that "normal" in
            // justified mode is the same as "full" in ragged-right).
            hyphenPenalty *= 0.25;
        } else {
            // Line penalty is zero for justified text.
            mLinePenalty = std::max(mLinePenalty, hyphenPenalty * LINE_PENALTY_MULTIPLIER);
        }
    }

    size_t current = (size_t) mWordBreaker.current();
    size_t afterWord = start;
    size_t lastBreak = start;
    ParaWidth lastBreakWidth = mWidth;
    ParaWidth postBreak = mWidth;
    size_t postSpaceCount = mSpaceCount;
    MinikinExtent extent = {0.0, 0.0, 0.0};
    for (size_t i = start; i < end; i++) {
        uint16_t c = mTextBuf[i];
        if (c == CHAR_TAB) {
            mWidth = mPreBreak + mTabStops.nextTab(mWidth - mPreBreak);
            if (mFirstTabIndex == INT_MAX) {
                mFirstTabIndex = (int)i;
            }
            // fall back to greedy; other modes don't know how to deal with tabs
            mStrategy = kBreakStrategy_Greedy;
        } else {
            if (isWordSpace(c)) mSpaceCount += 1;
            mWidth += mCharWidths[i];
            extent.extendBy(mCharExtents[i]);
            if (!isLineEndSpace(c)) {
                postBreak = mWidth;
                postSpaceCount = mSpaceCount;
                afterWord = i + 1;
            }
        }
        if (i + 1 == current) {
            size_t wordStart = mWordBreaker.wordStart();
            size_t wordEnd = mWordBreaker.wordEnd();
            if (paint != nullptr && mHyphenator != nullptr &&
                    mHyphenationFrequency != kHyphenationFrequency_None &&
                    wordStart >= start && wordEnd > wordStart) {
                hyphenate(&mTextBuf[wordStart], wordEnd - wordStart);
#if VERBOSE_DEBUG
                std::string hyphenatedString;
                for (size_t j = wordStart; j < wordEnd; j++) {
                    if (mHyphBuf[j - wordStart] == HyphenationType::BREAK_AND_INSERT_HYPHEN) {
                        hyphenatedString.push_back('-');
                    }
                    // Note: only works with ASCII, should do UTF-8 conversion here
                    hyphenatedString.push_back(buffer()[j]);
                }
                ALOGD("hyphenated string: %s", hyphenatedString.c_str());
#endif

                // measure hyphenated substrings
                for (size_t j = wordStart; j < wordEnd; j++) {
                    HyphenationType hyph = mHyphBuf[j - wordStart];
                    if (hyph != HyphenationType::DONT_BREAK) {
                        paint->hyphenEdit = HyphenEdit::editForThisLine(hyph);
                        const float firstPartWidth = Layout::measureText(mTextBuf.data(),
                                lastBreak, j - lastBreak, mTextBuf.size(), bidiFlags, style,
                                *paint, typeface, nullptr, nullptr /* extent */);
                        ParaWidth hyphPostBreak = lastBreakWidth + firstPartWidth;

                        paint->hyphenEdit = HyphenEdit::editForNextLine(hyph);
                        const float secondPartWidth = Layout::measureText(mTextBuf.data(), j,
                                afterWord - j, mTextBuf.size(), bidiFlags, style, *paint,
                                typeface, nullptr, nullptr /* extent */);
                        ParaWidth hyphPreBreak = postBreak - secondPartWidth;

                        addWordBreak(j, hyphPreBreak, hyphPostBreak, postSpaceCount, postSpaceCount,
                                extent, hyphenPenalty, hyph);
                        extent.reset();

                        paint->hyphenEdit = HyphenEdit::NO_EDIT;
                    }
                }
            }

            // Skip break for zero-width characters inside replacement span
            if (paint != nullptr || current == end || mCharWidths[current] > 0) {
                float penalty = hyphenPenalty * mWordBreaker.breakBadness();
                addWordBreak(current, mWidth, postBreak, mSpaceCount, postSpaceCount, extent,
                        penalty, HyphenationType::DONT_BREAK);
                extent.reset();
            }
            lastBreak = current;
            lastBreakWidth = mWidth;
            current = (size_t)mWordBreaker.next();
        }
    }

    return width;
}

// add a word break (possibly for a hyphenated fragment), and add desperate breaks if
// needed (ie when word exceeds current line width)
void LineBreaker::addWordBreak(size_t offset, ParaWidth preBreak, ParaWidth postBreak,
        size_t preSpaceCount, size_t postSpaceCount, MinikinExtent extent,
        float penalty, HyphenationType hyph) {
    Candidate cand;
    ParaWidth width = mCandidates.back().preBreak;
    if (postBreak - width > currentLineWidth()) {
        // Add desperate breaks.
        // Note: these breaks are based on the shaping of the (non-broken) original text; they
        // are imprecise especially in the presence of kerning, ligatures, and Arabic shaping.
        size_t i = mCandidates.back().offset;
        width += mCharWidths[i++];
        for (; i < offset; i++) {
            float w = mCharWidths[i];
            if (w > 0) {
                cand.offset = i;
                cand.preBreak = width;
                cand.postBreak = width;
                // postSpaceCount doesn't include trailing spaces
                cand.preSpaceCount = postSpaceCount;
                cand.postSpaceCount = postSpaceCount;
                cand.extent = mCharExtents[i];
                cand.penalty = SCORE_DESPERATE;
                cand.hyphenType = HyphenationType::BREAK_AND_DONT_INSERT_HYPHEN;
#if VERBOSE_DEBUG
                ALOGD("desperate cand: %zd %g:%g",
                        mCandidates.size(), cand.postBreak, cand.preBreak);
#endif
                addCandidate(cand);
                width += w;
            }
        }
    }

    cand.offset = offset;
    cand.preBreak = preBreak;
    cand.postBreak = postBreak;
    cand.penalty = penalty;
    cand.preSpaceCount = preSpaceCount;
    cand.postSpaceCount = postSpaceCount;
    cand.extent = extent;
    cand.hyphenType = hyph;
#if VERBOSE_DEBUG
    ALOGD("cand: %zd %g:%g", mCandidates.size(), cand.postBreak, cand.preBreak);
#endif
    addCandidate(cand);
}

// Find the needed extent between the start and end ranges. start and end are inclusive.
MinikinExtent LineBreaker::computeMaxExtent(size_t start, size_t end) const {
    MinikinExtent res = mCandidates[end].extent;
    for (size_t j = start; j < end; j++) {
        res.extendBy(mCandidates[j].extent);
    }
    return res;
}

// Helper method for addCandidate()
void LineBreaker::pushGreedyBreak() {
    const Candidate& bestCandidate = mCandidates[mBestBreak];
    pushBreak(bestCandidate.offset, bestCandidate.postBreak - mPreBreak,
            computeMaxExtent(mLastBreak + 1, mBestBreak),
            mLastHyphenation | HyphenEdit::editForThisLine(bestCandidate.hyphenType));
    mBestScore = SCORE_INFTY;
#if VERBOSE_DEBUG
    ALOGD("break: %d %g", mBreaks.back(), mWidths.back());
#endif
    mLastBreak = mBestBreak;
    mPreBreak = bestCandidate.preBreak;
    mLastHyphenation = HyphenEdit::editForNextLine(bestCandidate.hyphenType);
}

// TODO performance: could avoid populating mCandidates if greedy only
void LineBreaker::addCandidate(Candidate cand) {
    const size_t candIndex = mCandidates.size();
    mCandidates.push_back(cand);

    // mLastBreak is the index of the last line break we decided to do in mCandidates,
    // and mPreBreak is its preBreak value. mBestBreak is the index of the best line breaking candidate
    // we have found since then, and mBestScore is its penalty.
    if (cand.postBreak - mPreBreak > currentLineWidth()) {
        // This break would create an overfull line, pick the best break and break there (greedy)
        if (mBestBreak == mLastBreak) {
            // No good break has been found since last break. Break here.
            mBestBreak = candIndex;
        }
        pushGreedyBreak();
    }

    while (mLastBreak != candIndex && cand.postBreak - mPreBreak > currentLineWidth()) {
        // We should rarely come here. But if we are here, we have broken the line, but the
        // remaining part still doesn't fit. We now need to break at the second best place after the
        // last break, but we have not kept that information, so we need to go back and find it.
        //
        // In some really rare cases, postBreak - preBreak of a candidate itself may be over the
        // current line width. We protect ourselves against an infinite loop in that case by
        // checking that we have not broken the line at this candidate already.
        for (size_t i = mLastBreak + 1; i < candIndex; i++) {
            const float penalty = mCandidates[i].penalty;
            if (penalty <= mBestScore) {
                mBestBreak = i;
                mBestScore = penalty;
            }
        }
        if (mBestBreak == mLastBreak) {
            // We didn't find anything good. Break here.
            mBestBreak = candIndex;
        }
        pushGreedyBreak();
    }

    if (cand.penalty <= mBestScore) {
        mBestBreak = candIndex;
        mBestScore = cand.penalty;
    }
}

void LineBreaker::pushBreak(int offset, float width, MinikinExtent extent, uint8_t hyphenEdit) {
    mBreaks.push_back(offset);
    mWidths.push_back(width);
    mAscents.push_back(extent.ascent);
    mDescents.push_back(extent.descent);
    int flags = (mFirstTabIndex < mBreaks.back()) << kTab_Shift;
    flags |= hyphenEdit;
    mFlags.push_back(flags);
    mFirstTabIndex = INT_MAX;
}

void LineBreaker::addReplacement(size_t start, size_t end, float width) {
    mCharWidths[start] = width;
    std::fill(&mCharWidths[start + 1], &mCharWidths[end], 0.0f);
    // TODO: Get the extents information from the caller.
    std::fill(&mCharExtents[start], &mCharExtents[end], (MinikinExtent) {0.0f, 0.0f, 0.0f});
    addStyleRun(nullptr, nullptr, FontStyle(), start, end, false);
}

// Get the width of a space. May return 0 if there are no spaces.
// Note: if there are multiple different widths for spaces (for example, because of mixing of
// fonts), it's only guaranteed to pick one.
float LineBreaker::getSpaceWidth() const {
    for (size_t i = 0; i < mTextBuf.size(); i++) {
        if (isWordSpace(mTextBuf[i])) {
            return mCharWidths[i];
        }
    }
    return 0.0f;
}

float LineBreaker::currentLineWidth() const {
    return mLineWidthDelegate->getLineWidth(mBreaks.size());
}

void LineBreaker::computeBreaksGreedy() {
    // All breaks but the last have been added in addCandidate already.
    size_t nCand = mCandidates.size();
    if (nCand == 1 || mLastBreak != nCand - 1) {
        pushBreak(mCandidates[nCand - 1].offset, mCandidates[nCand - 1].postBreak - mPreBreak,
                computeMaxExtent(mLastBreak + 1, nCand - 1),
                mLastHyphenation);
        // don't need to update mBestScore, because we're done
#if VERBOSE_DEBUG
        ALOGD("final break: %d %g", mBreaks.back(), mWidths.back());
#endif
    }
}

// Follow "prev" links in mCandidates array, and copy to result arrays.
void LineBreaker::finishBreaksOptimal() {
    // clear existing greedy break result
    mBreaks.clear();
    mWidths.clear();
    mAscents.clear();
    mDescents.clear();
    mFlags.clear();

    size_t nCand = mCandidates.size();
    size_t prev;
    for (size_t i = nCand - 1; i > 0; i = prev) {
        prev = mCandidates[i].prev;
        mBreaks.push_back(mCandidates[i].offset);
        mWidths.push_back(mCandidates[i].postBreak - mCandidates[prev].preBreak);
        MinikinExtent extent = computeMaxExtent(prev + 1, i);
        mAscents.push_back(extent.ascent);
        mDescents.push_back(extent.descent);
        int flags = HyphenEdit::editForThisLine(mCandidates[i].hyphenType);
        if (prev > 0) {
            flags |= HyphenEdit::editForNextLine(mCandidates[prev].hyphenType);
        }
        mFlags.push_back(flags);
    }
    std::reverse(mBreaks.begin(), mBreaks.end());
    std::reverse(mWidths.begin(), mWidths.end());
    std::reverse(mFlags.begin(), mFlags.end());
}

void LineBreaker::computeBreaksOptimal() {
    size_t active = 0;
    size_t nCand = mCandidates.size();
    float width = mLineWidthDelegate->getLineWidth(0);
    float maxShrink = mJustified ? SHRINKABILITY * getSpaceWidth() : 0.0f;
    std::vector<size_t> lineNumbers;
    lineNumbers.reserve(nCand);
    lineNumbers.push_back(0);  // The first candidate is always at the first line.

    // "i" iterates through candidates for the end of the line.
    for (size_t i = 1; i < nCand; i++) {
        bool atEnd = i == nCand - 1;
        float best = SCORE_INFTY;
        size_t bestPrev = 0;

        size_t lineNumberLast = lineNumbers[active];
        width = mLineWidthDelegate->getLineWidth(lineNumberLast);

        ParaWidth leftEdge = mCandidates[i].postBreak - width;
        float bestHope = 0;

        // "j" iterates through candidates for the beginning of the line.
        for (size_t j = active; j < i; j++) {
            size_t lineNumber = lineNumbers[j];
            if (lineNumber != lineNumberLast) {
                float widthNew = mLineWidthDelegate->getLineWidth(lineNumber);
                if (widthNew != width) {
                    leftEdge = mCandidates[i].postBreak - width;
                    bestHope = 0;
                    width = widthNew;
                }
                lineNumberLast = lineNumber;
            }
            float jScore = mCandidates[j].score;
            if (jScore + bestHope >= best) continue;
            float delta = mCandidates[j].preBreak - leftEdge;

            // compute width score for line

            // Note: the "bestHope" optimization makes the assumption that, when delta is
            // non-negative, widthScore will increase monotonically as successive candidate
            // breaks are considered.
            float widthScore = 0.0f;
            float additionalPenalty = 0.0f;
            if ((atEnd || !mJustified) && delta < 0) {
                widthScore = SCORE_OVERFULL;
            } else if (atEnd && mStrategy != kBreakStrategy_Balanced) {
                // increase penalty for hyphen on last line
                additionalPenalty = LAST_LINE_PENALTY_MULTIPLIER * mCandidates[j].penalty;
            } else {
                widthScore = delta * delta;
                if (delta < 0) {
                    if (-delta < maxShrink *
                            (mCandidates[i].postSpaceCount - mCandidates[j].preSpaceCount)) {
                        widthScore *= SHRINK_PENALTY_MULTIPLIER;
                    } else {
                        widthScore = SCORE_OVERFULL;
                    }
                }
            }

            if (delta < 0) {
                active = j + 1;
            } else {
                bestHope = widthScore;
            }

            float score = jScore + widthScore + additionalPenalty;
            if (score <= best) {
                best = score;
                bestPrev = j;
            }
        }
        mCandidates[i].score = best + mCandidates[i].penalty + mLinePenalty;
        mCandidates[i].prev = bestPrev;
        lineNumbers.push_back(lineNumbers[bestPrev] + 1);
#if VERBOSE_DEBUG
        ALOGD("break %zd: score=%g, prev=%zd", i, mCandidates[i].score, mCandidates[i].prev);
#endif
    }
    finishBreaksOptimal();
}

size_t LineBreaker::computeBreaks() {
    if (mStrategy == kBreakStrategy_Greedy) {
        computeBreaksGreedy();
    } else {
        computeBreaksOptimal();
    }
    return mBreaks.size();
}

void LineBreaker::finish() {
    mWordBreaker.finish();
    mWidth = 0;
    mCandidates.clear();
    mBreaks.clear();
    mWidths.clear();
    mAscents.clear();
    mDescents.clear();
    mFlags.clear();
    if (mTextBuf.size() > MAX_TEXT_BUF_RETAIN) {
        mTextBuf.clear();
        mTextBuf.shrink_to_fit();
        mCharWidths.clear();
        mCharWidths.shrink_to_fit();
        mCharExtents.clear();
        mCharExtents.shrink_to_fit();
        mHyphBuf.clear();
        mHyphBuf.shrink_to_fit();
        mCandidates.shrink_to_fit();
        mBreaks.shrink_to_fit();
        mWidths.shrink_to_fit();
        mAscents.shrink_to_fit();
        mDescents.shrink_to_fit();
        mFlags.shrink_to_fit();
    }
    mStrategy = kBreakStrategy_Greedy;
    mHyphenationFrequency = kHyphenationFrequency_Normal;
    mLinePenalty = 0.0f;
    mJustified = false;
    mLineWidthDelegate.reset();
}

}  // namespace minikin
