# Kindle Downloader
I wrote this App to simplify downloading my Kinlde library. A bit of background is appropriate - I have more than 30,000 books I have purchased on Amazon Kindle and I have more than 8 kindle devices lying around, excluding multiple IPads with Kindle installed. I keep a list of books I am reading and I may pick up a book after more than a year. I read on different devices and I started to run into a problem of Authors limiting the number of devices you can have a book downloaded on. It got bad enough I took to downloading any books I am reading on the Kindle App for PC and uploading the book to Google Play books.

This has worked well for years, and I have used Kindle for PC Version 1.17.44170. However, Amazon is tightening its DRM and starting this year newly released books use KFX and will no longer download on the older Kindle for PC versions. I can see this being extended to the whole library in time and not just new books and I have decided to download all my kindle books while I still can.

I found some programs that purported to automate downloading your kindle library but none worked. I tried to manually download my library, but you have to click one book at a time..... and I have a life. This App automates that process, all it does is click on the Kindle for PC app repeatedly and downloads your books. It is based on one specific Kindle for PC behavior - when your list of books is sorted by RECENT, downloading a book takes it to the top of the list and the list moves down - so you can keep clicking on the same spot to download all your books.

Out of more than 30,000 books, 44 books did not download on the PC app and I downloaded these books from the Amazon website directly. I was able to import all books into Calibre and 32 could not be converted into the Epub format.

I wrote this in Visual C++ Community Edition and my installation is a tad borked. It won't let me create new dialog boxes and I ended up modifying the About dialog box for my needs. The whole thing took about an hour to write, it works, and I am not inclined to put in any more effort. This is a very simple program that does one thing reasonably well. I am uploading it because I found it very useful and others may too. I can think of many simple changes that would improve utility, but life calls.

How To Use
1. Kindle for PC must be in List View and Sorterd by Recent, scroll to the bottom of the list
2. Launch KindleDownloader 
3. Click File Menu and select KindleDownloader
4. On the new screen, Click Target button - you now have 3 seconds to move your cursor to the Kindle for PC App, over any title
5. KindleDownloader will click download about once every 4 seconds
6. Press F9 to stop the program
7. KindleDownloader will attempt to download 1000 books each run

Troubleshooting
1. Some books take longer than 4 seconds to download. These books will go into a cycle of starting and stopping the download. Press F9 and download these books manually
2. Race conditions - Race conditions occur when Kindle for PC is updating its list by moving the downloaded book up while KindleDownloader is clicking. This sometimes leads to the book being opened or the list moving to the top. Press F9, go back to the bottom of the list and restart. Note, most Race conditions do nothing and the program continues as expected with no interventions.
3. Kindle for PC tends to close after downloading about 1000 books. I presume this is caused by problems with accumulating Race condition errors.
4. KFX titles - "Your Kindle app required an update to view this content. Click here to download and install the free update (supported OS: Windows 10, Windows 8).". Skip these titles. I had 44 out of more than 30,000 that mandated KFX. I could download them directly from the Amazon website and import into Calibre.


