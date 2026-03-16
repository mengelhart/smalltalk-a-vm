"Kernel methods for False.
 Do NOT duplicate conditional methods installed by bootstrap.c step 7:
   ifTrue:, ifFalse:, ifTrue:ifFalse:"

!False methodsFor: 'logical operations'!
not
	^ true!
& aBoolean
	^ false!
| aBoolean
	^ aBoolean! !

!False methodsFor: 'printing'!
printString
	^ 'false'! !
