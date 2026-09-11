-- Text for the Bot Manager's Help button. It's plain text between the [[ and ]]
-- markers below -- edit it however you like.

AHSim = AHSim or {}

AHSim.helpText = [[
AuctionSim - First-Time Setup
=============================

AuctionSim runs a bot character that lists and buys items on the Auction House 
so a low-population realm still has a busy AH.

Everything in this window is saved to auctionsim.conf. You do not need to edit
that file by hand.


1. Requirements
---------------
- A character for the bot to use. A dedicated character on its own account is
  best, but any character works - including the one you are logged in on right
  now.
- Two-side auction interaction must be OFF in worldserver.conf:
      AllowTwoSide.Interaction.Auction = 0
  The module refuses to start otherwise.
- You must be a GM to open this window (/auctionsim or /ahsim).


2. Point the module at the bot character(s)
-------------------------------------------
- Edit auctionsim.conf directly and set one (or both) of:
      AuctionSim.BotCharacterIDs = 12345
      AuctionSim.BotAccountIDs = 67
  BotCharacterIDs takes one or more character ids (comma-separated for more
  than one). BotAccountIDs takes one or more account ids; every character on
  each listed account is automatically added to the bot roster.
- Restart the server, or run a live reload if your build exposes one, to pick
  up the change.
- There's no in-addon way to look this up by character name anymore -- with
  multiple bot characters supported, a "type one name" popup stopped making
  sense. Use your account/character DB to find the ids you need.


3. Enable the module
--------------------
- Tick "Enabled". It saves immediately and starts the bot.
- Optionally tick "Startup Scan" to run a scan automatically on every server
  start.


4. Choose what the bot lists
----------------------------
In the "Listing Multipliers" grid on the right:

- Max Required Level / Max Item Level: the bot skips items above these. 0 means
  no limit.
- The bot works out how full to keep each category and quality from real auction
  scan data. Each grid cell scales that target: 1 matches the real market, 1.5
  keeps it 50% fuller, 0.5 half as full, 0 disables that cell. Enter a plain
  number like 1, 1.5 or 0.25.
- The scan data was taken from a realm with a saturated auction house, so the
  default values are scaled down.
- When editing a cell you must press enter to apply the change.
- Click "Apply" to send your changes, then "Save To File" to write them to
  auctionsim.conf.
- "Refresh" reloads the values from the server and discards unsaved edits.

5. First run
------------
- Click "Scan". The bot lists new auctions and queues items to buy. Queued buys
  are spread over time, not done all at once.
- Note: Searching the auction house after running a scan while logged in as the
  bot character can take a little while for the auction db to update if there are
  a lot of new auctions.
- After that the module scans on its own on a timer.


Button reference
----------------
Scan            List new auctions and queue buys now.
Delete          Remove every auction the bot currently has listed.
Show Queue      Show the buy queue size and when the next and last buy are due.
Clean Over Cap  Remove bot auctions that are now above the level caps.
Run Tests       Run the module's built-in self-tests; output goes to Results.
Force Buy       Immediately buy the next queued purchase, without waiting for
                its rolled buy time. Useful for testing a buy cycle on demand,
                or draining the queue without deleting the bot's own listings
                -- this actually buys the auction, same as it would have on
                its own, just not waiting around for it.
Help            This window.


Notes
-----
- Mail the bot would get from its own auctions is discarded automatically.
- Everything set here is written to auctionsim.conf, so it survives a restart.
]]
