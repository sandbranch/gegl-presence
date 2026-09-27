# Test photos

`fetch.py` downloads nine photos from Wikimedia Commons, all CC0 or
public domain (the license was read on each photo's Commons page, see
`manifest.json` for author, license and page). Commons serves standard
thumbnail sizes, so they arrive 2500 to 3840 pixels wide;
`alps-grenoble.jpg` comes at its full 6240 x 4160 (26 megapixels) for
the timing on a real photo. They stay out of git to keep the repository
small; `fetch.py` makes the same set anywhere.

    tests/images/fetch.py

| File | What | Author | License |
|---|---|---|---|
| hazy-blue-slopes.jpg | blue haze over wooded slopes | Greg Rakozy | CC0 |
| hazy-mountains-fire.jpg | haze from grassland fires, Monterrey | Doggo19292 | public domain |
| hazy-kathmandu.jpg | Kathmandu Valley under haze | Krishna k. sahh | CC0 |
| hazy-summer-valley.jpg | summer haze over a mountain valley | Rosan Harmens | CC0 |
| hazy-turquoise.jpg | turquoise haze over mountains | Ju On | CC0 |
| alps-grenoble.jpg | the Western Alps from Grenoble | Dktue | CC0 |
| face-portrait.jpg | a face, close | William Stitt | CC0 |
| freckles.jpg | curly hair and freckles | Jeremy Bishop | CC0 |
| bark-oak.jpg | oak bark | Kurt Kaiser | CC0 |

No other photos are used by the tests, and the user's own photos never.
