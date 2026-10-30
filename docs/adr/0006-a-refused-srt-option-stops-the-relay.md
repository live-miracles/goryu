# A refused SRT option stops the relay at startup

Every SRT option Goryu applies is checked, and a refusal by libsrt ends startup with an error instead of being skipped. An earlier version ignored libsrt's refusal of a too-short passphrase (it must be 10 to 79 bytes), so a configured but invalid passphrase left the whole relay, input and output, running without encryption and accepting callers that sent none. For a live relay, refusing to start is the lesser failure.
