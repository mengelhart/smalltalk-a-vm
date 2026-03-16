"Kernel methods for True.
 Do NOT duplicate conditional methods installed by bootstrap.c step 7:
   ifTrue:, ifFalse:, ifTrue:ifFalse:"

!True methodsFor: 'logical operations'!
not
	^ false!
& aBoolean
	^ aBoolean!
| aBoolean
	^ true! !

!True methodsFor: 'printing'!
printString
	^ 'true'! !
