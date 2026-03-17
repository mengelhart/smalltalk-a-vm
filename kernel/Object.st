"Kernel methods for Object.
 Do NOT duplicate primitive methods installed by bootstrap.c step 7:
   ==, class, basicAt:, basicAt:put:, basicSize, hash, become:,
   instVarAt:, instVarAt:put:, identityHash, shallowCopy, yourself,
   respondsTo:, doesNotUnderstand:, initialize"

!Object methodsFor: 'testing'!
isNil
	^ false!
notNil
	^ true!
isNumber
	^ false!
isInteger
	^ false! !

!Object methodsFor: 'comparing'!
= anObject
	^ self == anObject!
~= anObject
	^ (self = anObject) not! !

!Object methodsFor: 'error handling'!
subclassResponsibility
	self error: 'subclass responsibility'!
error: aString
	Error new messageText: aString; signal! !
